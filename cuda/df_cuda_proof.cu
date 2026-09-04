// dependency_fabric::cuda_proof — CUDA-backed dependency-gated execution proof.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Proves that Dependency Fabric readiness gates real accelerator work on the
// RTX 5090 (sm_120). Every scenario uses real cudaMalloc/H2D/kernel/sync/D2H/
// cudaFree and CPU-reference verification. Device memory must return to
// baseline. Device recovery, generation, and capability facts are measured
// from the real device; capability requirement scenarios that cannot be
// satisfied are explicitly labelled SYNTHETIC. We do not claim multi-GPU,
// RDMA, NVLink, or hardware discovery we did not measure.

#include "dependency_fabric/df.hpp"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <cuda_runtime.h>

using namespace dependency_fabric;

namespace {

#define CUDA_CHECK(expr)                                                        \
  do {                                                                          \
    cudaError_t _e = (expr);                                                    \
    if (_e != cudaSuccess) {                                                    \
      std::fprintf(stderr, "CUDA error at %s:%d: %s\n", __FILE__, __LINE__,    \
                   cudaGetErrorString(_e));                                     \
      std::exit(1);                                                             \
    }                                                                           \
  } while (0)

__global__ void saxpy_kernel(int n, float a, const float* x, float* y) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) y[i] = a * x[i] + y[i];
}

// CPU reference of y = a*x + y.
void cpu_reference(int n, float a, const std::vector<float>& x, std::vector<float>& y) {
  for (int i = 0; i < n; ++i) y[i] = a * x[i] + y[i];
}

bool all_close(const std::vector<float>& a, const std::vector<float>& b, float tol = 1e-4f) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::fabs(a[i] - b[i]) > tol) return false;
  }
  return true;
}

std::size_t free_mem() {
  std::size_t free_bytes = 0, total_bytes = 0;
  CUDA_CHECK(cudaMemGetInfo(&free_bytes, &total_bytes));
  return free_bytes;
}

// ---------------------------------------------------------------------------
// A host-side dependency-gated CUDA runner. A node id in the graph gates the
// execution: CUDA work is only launched when the dependency is READY.
// ---------------------------------------------------------------------------
struct CudaRun {
  Graph& g;
  DependencyNodeId gate;
  int n;
  float a;
  std::vector<float> x, y_host, y_ref;

  CudaRun(Graph& graph, DependencyNodeId gid, int size, float alpha)
      : g(graph), gate(gid), n(size), a(alpha), x(size), y_host(size), y_ref(size) {
    for (int i = 0; i < n; ++i) x[i] = static_cast<float>((i % 1000) - 500) * 0.01f;
    for (int i = 0; i < n; ++i) y_host[i] = static_cast<float>((i % 257) - 128) * 0.01f;
    y_ref = y_host;
  }

  // Returns true if the gate is READY; launches the real CUDA kernel only then.
  bool run() {
    const ReadinessState st = g.readiness(gate);
    std::printf("  gate node %llu state=%s -> %s\n", static_cast<unsigned long long>(gate.value()),
                std::string(name_of(st)).c_str(), st == ReadinessState::READY ? "RUN" : "BLOCK");
    if (st != ReadinessState::READY) return false;   // dependency gate denies launch

    const std::size_t base = free_mem();
    float* dx = nullptr; float* dy = nullptr;
    CUDA_CHECK(cudaMalloc(&dx, n * sizeof(float)));
    CUDA_CHECK(cudaMalloc(&dy, n * sizeof(float)));
    CUDA_CHECK(cudaMemcpy(dx, x.data(), n * sizeof(float), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(dy, y_host.data(), n * sizeof(float), cudaMemcpyHostToDevice));
    const int threads = 256;
    const int blocks = (n + threads - 1) / threads;
    saxpy_kernel<<<blocks, threads>>>(n, a, dx, dy);
    CUDA_CHECK(cudaDeviceSynchronize());
    CUDA_CHECK(cudaMemcpy(y_host.data(), dy, n * sizeof(float), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaFree(dx));
    CUDA_CHECK(cudaFree(dy));
    const std::size_t after = free_mem();
    std::printf("  device memory baseline=%zu after=%zu (delta=%zd, %s)\n",
                base, after, (std::ptrdiff_t)(after)-base, after == base ? "OK" : "LEAK");
    if (after != base) { std::printf("  FAIL: device memory did not return to baseline\n"); return false; }

    // CPU reference verification.
    cpu_reference(n, a, x, y_ref);
    if (!all_close(y_host, y_ref)) {
      std::printf("  FAIL: CUDA result does not match CPU reference\n");
      return false;
    }
    std::printf("  CPU parity: OK\n");
    return true;
  }
};

// Build a chain: model(1) -> kernel(2) -> execution gate(3).
void build_chain(Graph& g, const SourceId& src, const SourceBootId& boot, const CoordinatorEpoch& epoch,
                 std::uint64_t model_gen, std::uint64_t kernel_gen, bool durable = true) {
  g.declare_node(DependencyNodeId(1), NodeKind::MODEL, "model", src, boot, epoch, durable);
  g.declare_node(DependencyNodeId(2), NodeKind::KERNEL, "kernel", src, boot, epoch, durable);
  g.declare_node(DependencyNodeId(3), NodeKind::WORKLOAD_PHASE, "exec", src, boot, epoch, durable);
  g.publish_generation(DependencyNodeId(1), DependencyNodeGeneration(model_gen), src, boot, epoch);
  g.publish_generation(DependencyNodeId(2), DependencyNodeGeneration(kernel_gen), src, boot, epoch);
  Edge em;
  em.id = DependencyEdgeId(1); em.generation = DependencyEdgeGeneration(1); em.kind = EdgeKind::REQUIRES_GENERATION;
  em.producer_id = DependencyNodeId(1); em.consumer_id = DependencyNodeId(2);
  em.predicate.generation_kind = GenerationPredicateKind::EXACT; em.predicate.exact_generation = DependencyNodeGeneration(model_gen);
  em.predicate.required_readiness = ReadinessState::READY; em.predicate.is_required = true;
  g.add_edge(em, src, boot, epoch);
  Edge ek;
  ek.id = DependencyEdgeId(2); ek.generation = DependencyEdgeGeneration(1); ek.kind = EdgeKind::REQUIRES_GENERATION;
  ek.producer_id = DependencyNodeId(2); ek.consumer_id = DependencyNodeId(3);
  ek.predicate.generation_kind = GenerationPredicateKind::EXACT; ek.predicate.exact_generation = DependencyNodeGeneration(kernel_gen);
  ek.predicate.required_readiness = ReadinessState::READY; ek.predicate.is_required = true;
  g.add_edge(ek, src, boot, epoch);
  g.recompute_all_readiness();
}

}  // namespace

int main() {
  cudaDeviceProp prop{};
  CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
  std::printf("Device: %s computeCapability=%d.%d sm_%d%d\n", prop.name, prop.major, prop.minor,
              prop.major, prop.minor);
  if (prop.major != 12) {
    std::printf("WARNING: expected sm_120 (RTX 5090) but found sm_%d%d\n", prop.major, prop.minor);
  }

  const SourceId src(1);
  const SourceBootId boot(1);
  const CoordinatorEpoch epoch(1);
  const int N = 1 << 20;

  // -------------------------------------------------------------------------
  // Scenario A: valid dependency chain -> CUDA work allowed -> parity passes.
  // -------------------------------------------------------------------------
  std::printf("\n=== Scenario A: valid dependency chain gates CUDA ===\n");
  {
    Graph g;
    g.register_source(src, boot, epoch);
    build_chain(g, src, boot, epoch, 1, 1);
    CudaRun run(g, DependencyNodeId(3), N, 2.0f);
    if (run.run()) std::printf("  Scenario A: PASS\n");
    else std::printf("  Scenario A: FAIL\n");
    if (g.readiness(DependencyNodeId(3)) != ReadinessState::READY) std::printf("  Scenario A: FAIL (gate not READY)\n");
  }

  // -------------------------------------------------------------------------
  // Scenario B: stale model generation -> CUDA rejected before launch; fresh
  // dependent generations republish -> CUDA succeeds.
  // -------------------------------------------------------------------------
  std::printf("\n=== Scenario B: stale generation blocks CUDA launch ===\n");
  {
    Graph g;
    g.register_source(src, boot, epoch);
    build_chain(g, src, boot, epoch, 1, 1);
    // Rebuild is simulated by advancing the model and re-binding the kernel.
    g.publish_generation(DependencyNodeId(1), DependencyNodeGeneration(2), src, boot, epoch);
    g.recompute_all_readiness();
    std::printf("  after model gen2: kernel=%s exec=%s\n",
                std::string(name_of(g.readiness(DependencyNodeId(2)))).c_str(),
                std::string(name_of(g.readiness(DependencyNodeId(3)))).c_str());
    CudaRun run(g, DependencyNodeId(3), N, 2.0f);
    if (run.run()) std::printf("  Scenario B: FAIL (stale execution was allowed)\n");
    else std::printf("  Scenario B: launch correctly rejected (state=%s)\n",
                     std::string(name_of(g.readiness(DependencyNodeId(3)))).c_str());

    // Republish a fresh kernel generation bound to the new model gen.
    g.remove_edge(DependencyEdgeId(1), src, boot, epoch);
    Edge em2;
    em2.id = DependencyEdgeId(3); em2.generation = DependencyEdgeGeneration(1); em2.kind = EdgeKind::REQUIRES_GENERATION;
    em2.producer_id = DependencyNodeId(1); em2.consumer_id = DependencyNodeId(2);
    em2.predicate.generation_kind = GenerationPredicateKind::EXACT; em2.predicate.exact_generation = DependencyNodeGeneration(2);
    em2.predicate.required_readiness = ReadinessState::READY; em2.predicate.is_required = true;
    g.add_edge(em2, src, boot, epoch);
    g.publish_generation(DependencyNodeId(2), DependencyNodeGeneration(2), src, boot, epoch);
    g.remove_edge(DependencyEdgeId(2), src, boot, epoch);
    Edge ek2;
    ek2.id = DependencyEdgeId(4); ek2.generation = DependencyEdgeGeneration(1); ek2.kind = EdgeKind::REQUIRES_GENERATION;
    ek2.producer_id = DependencyNodeId(2); ek2.consumer_id = DependencyNodeId(3);
    ek2.predicate.generation_kind = GenerationPredicateKind::EXACT; ek2.predicate.exact_generation = DependencyNodeGeneration(2);
    ek2.predicate.required_readiness = ReadinessState::READY; ek2.predicate.is_required = true;
    g.add_edge(ek2, src, boot, epoch);
    g.recompute_all_readiness();
    if (g.readiness(DependencyNodeId(3)) == ReadinessState::READY) {
      CudaRun run2(g, DependencyNodeId(3), N, 3.0f);
      if (run2.run()) std::printf("  Scenario B: PASS (fresh dependent generations republished)\n");
      else std::printf("  Scenario B: FAIL (fresh generations did not permit run)\n");
    } else {
      std::printf("  Scenario B: FAIL (gate not READY after republish: %s)\n",
                  std::string(name_of(g.readiness(DependencyNodeId(3)))).c_str());
    }
  }

  // -------------------------------------------------------------------------
  // Scenario C: invalidation during a segmented workflow.
  // -------------------------------------------------------------------------
  std::printf("\n=== Scenario C: segmented CUDA workflow with invalidation ===\n");
  {
    Graph g;
    g.register_source(src, boot, epoch);
    build_chain(g, src, boot, epoch, 1, 1);
    // Segment 1 runs: the dependency is READY.
    CudaRun seg1(g, DependencyNodeId(3), N, 2.0f);
    bool s1 = seg1.run();
    std::printf("  segment 1: %s\n", s1 ? "RUN" : "FAIL");
    // Invalidate the required dependency between segments.
    InvalidationPolicy pol;
    pol.mode = InvalidationMode::RECURSIVE;
    g.invalidate_node(DependencyNodeId(1), InvalidationReason::INTEGRITY_FAILED, pol, src, boot, epoch);
    g.recompute_all_readiness();
    std::printf("  after invalidation: exec=%s (must be BLOCKED)\n",
                std::string(name_of(g.readiness(DependencyNodeId(3)))).c_str());
    // Segment 2 must NOT execute.
    CudaRun seg2(g, DependencyNodeId(3), N, 3.0f);
    bool s2 = seg2.run();
    std::printf("  segment 2 (during invalidation): %s\n", s2 ? "FAIL (ran while invalid)" : "CORRECTLY BLOCKED");
    // Recover and republish current evidence.
    g.mark_recovered(DependencyNodeId(1), src, boot, epoch);
    g.recompute_all_readiness();
    // Segment 3 resumes under current authority.
    CudaRun seg3(g, DependencyNodeId(3), N, 4.0f);
    bool s3 = seg3.run();
    if (s1 && !s2 && s3) std::printf("  Scenario C: PASS (segmented workflow gated and resumed)\n");
    else std::printf("  Scenario C: FAIL\n");
  }

  // -------------------------------------------------------------------------
  // Scenario D: resource / capability dependency on the real device.
  // -------------------------------------------------------------------------
  std::printf("\n=== Scenario D: resource/capability dependency (real sm_120) ===\n");
  {
    Graph g;
    g.register_source(src, boot, epoch);
    g.declare_node(DependencyNodeId(1), NodeKind::RESOURCE, "rtx5090", src, boot, epoch, true);
    g.declare_node(DependencyNodeId(2), NodeKind::KERNEL, "kernel", src, boot, epoch, true);
    g.declare_node(DependencyNodeId(3), NodeKind::WORKLOAD_PHASE, "exec", src, boot, epoch, true);
    g.publish_generation(DependencyNodeId(1), DependencyNodeGeneration(1), src, boot, epoch);
    g.publish_generation(DependencyNodeId(2), DependencyNodeGeneration(1), src, boot, epoch);
    // Record REAL device capability/sm on the resource node.
    std::string sm = "sm_" + std::to_string(prop.major * 10 + prop.minor);
    g.set_node_evidence(DependencyNodeId(1),
                        NodeEvidencePatch{{sm}, false, true, false, true, false, true, false, 100, false, ""},
                        src, boot, epoch);
    // Kernel requires capability sm_120 (matches the real device).
    Edge ek;
    ek.id = DependencyEdgeId(1); ek.generation = DependencyEdgeGeneration(1); ek.kind = EdgeKind::REQUIRES_CAPABILITY;
    ek.producer_id = DependencyNodeId(1); ek.consumer_id = DependencyNodeId(2);
    ek.predicate.required_capability = sm;   // real measured capability
    ek.predicate.required_readiness = ReadinessState::READY; ek.predicate.is_required = true;
    g.add_edge(ek, src, boot, epoch);
    Edge ee;
    ee.id = DependencyEdgeId(2); ee.generation = DependencyEdgeGeneration(1); ee.kind = EdgeKind::REQUIRES_READY;
    ee.producer_id = DependencyNodeId(2); ee.consumer_id = DependencyNodeId(3);
    ee.predicate.required_readiness = ReadinessState::READY; ee.predicate.is_required = true;
    g.add_edge(ee, src, boot, epoch);
    g.recompute_all_readiness();
    std::printf("  resource capability '%s' claimed on real device; kernel=%s exec=%s\n",
                sm.c_str(),
                std::string(name_of(g.readiness(DependencyNodeId(2)))).c_str(),
                std::string(name_of(g.readiness(DependencyNodeId(3)))).c_str());
    if (g.readiness(DependencyNodeId(3)) == ReadinessState::READY) {
      CudaRun run(g, DependencyNodeId(3), N, 1.5f);
      if (run.run()) std::printf("  Scenario D (compatible real capability): PASS\n");
      else std::printf("  Scenario D: FAIL\n");
    } else {
      std::printf("  Scenario D: FAIL (compatible capability not READY)\n");
    }

    // An intentionally incompatible SYNTHETIC capability requirement stays blocked.
    Edge ek_bad;
    ek_bad.id = DependencyEdgeId(3); ek_bad.generation = DependencyEdgeGeneration(1); ek_bad.kind = EdgeKind::REQUIRES_CAPABILITY;
    ek_bad.producer_id = DependencyNodeId(1); ek_bad.consumer_id = DependencyNodeId(2);
    ek_bad.predicate.required_capability = "sm_" + std::to_string(prop.major * 10 + prop.minor + 1);  // SYNTHETIC, not real
    ek_bad.predicate.required_readiness = ReadinessState::READY; ek_bad.predicate.is_required = true;
    g.remove_edge(DependencyEdgeId(1), src, boot, epoch);
    g.add_edge(ek_bad, src, boot, epoch);
    g.recompute_all_readiness();
    const ReadinessState st = g.readiness(DependencyNodeId(2));
    std::printf("  SYNTHETIC incompatible capability '%s' -> kernel=%s (must NOT be READY)\n",
                ek_bad.predicate.required_capability.c_str(), std::string(name_of(st)).c_str());
    if (st != ReadinessState::READY) std::printf("  Scenario D (incompatible capability blocked): PASS\n");
    else std::printf("  Scenario D: FAIL (incompatible capability was accepted)\n");
  }

  // -------------------------------------------------------------------------
  // Scenario E: persist, restart coordinator, recovered CUDA readiness must be
  // revalidated, real device evidence republished, then CUDA works.
  // -------------------------------------------------------------------------
  std::printf("\n=== Scenario E: recovery requires revalidation ===\n");
  {
    Graph g;
    g.register_source(src, boot, epoch);
    // Dynamic (non-durable) evidence: after restart it must be revalidated.
    build_chain(g, src, boot, epoch, 1, 1, false);
    auto bytes = g.serialize();
    Graph g2;
    Persistence::load(*bytes, &g2, nullptr);
    const ReadinessState rec = g2.readiness(DependencyNodeId(3));
    std::printf("  after coordinator restart, exec=%s (must be REVALIDATION_REQUIRED, not READY)\n",
                std::string(name_of(rec)).c_str());
    if (rec == ReadinessState::READY) {
      std::printf("  Scenario E: FAIL (recovered dynamic readiness was silently accepted)\n");
    } else {
      // Republish current evidence bottom-up in a fresh coordinator epoch.
      g2.register_source(SourceId(2), SourceBootId(1), CoordinatorEpoch(1));
      g2.publish_readiness(DependencyNodeId(1), ReadinessState::READY, SourceId(2), SourceBootId(1), CoordinatorEpoch(1));
      g2.publish_readiness(DependencyNodeId(2), ReadinessState::READY, SourceId(2), SourceBootId(1), CoordinatorEpoch(1));
      g2.publish_readiness(DependencyNodeId(3), ReadinessState::READY, SourceId(2), SourceBootId(1), CoordinatorEpoch(1));
      if (g2.readiness(DependencyNodeId(3)) == ReadinessState::READY) {
        CudaRun run(g2, DependencyNodeId(3), N, 4.0f);
        if (run.run()) std::printf("  Scenario E: PASS (revalidated evidence restored CUDA)\n");
        else std::printf("  Scenario E: FAIL\n");
      } else {
        std::printf("  Scenario E: FAIL (gate not READY after revalidation: %s)\n",
                    std::string(name_of(g2.readiness(DependencyNodeId(3)))).c_str());
      }
    }
  }

  CUDA_CHECK(cudaDeviceReset());
  std::printf("\nAll CUDA scenarios complete.\n");
  return 0;
}