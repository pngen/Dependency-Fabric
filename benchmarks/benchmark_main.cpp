// dependency_fabric::benchmark — large-graph operation benchmarks.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Reports exact graph size/density per case and the cost of core operations:
// node registration, edge registration, readiness evaluation, blocker queries,
// ancestor/descendant traversal, invalidation & recovery propagation,
// generation supersession, dependency-set evaluation, persistence save/recover,
// and concurrent read queries.

#include "dependency_fabric/df.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace dependency_fabric;

namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

struct Case {
  std::size_t nodes;
  std::size_t edges;
};

void time_op(const char* name, std::size_t count, const double elapsed_ms) {
  const double per_us = elapsed_ms * 1000.0 / static_cast<double>(count);
  const double ops = static_cast<double>(count) / (elapsed_ms / 1000.0);
  std::printf("  %-38s %10zu ops  %9.2f ms  %10.2f us/op  %10.0f ops/s\n",
              name, count, elapsed_ms, per_us, ops);
}

void run_case(const Case& cs) {
  const std::size_t N = cs.nodes;
  const std::size_t E = cs.edges;
  std::printf("Case: %zu nodes, %zu edges (density=%.3f edges/node)\n", N, E,
              static_cast<double>(E) / static_cast<double>(N));

  const SourceId src(1);
  const SourceBootId boot(1);
  const CoordinatorEpoch epoch(1);
  Graph g;
  (void)g.register_source(src, boot, epoch);

  // Seed graph: publish a generation per node.
  auto t0 = Clock::now();
  for (std::size_t i = 1; i <= N; ++i) {
    (void)g.declare_node(DependencyNodeId(i), NodeKind::MODEL, "", src, boot, epoch);
    (void)g.publish_generation(DependencyNodeId(i), DependencyNodeGeneration(1), src, boot, epoch);
  }
  const double node_reg = ms(t0, Clock::now());
  time_op("node registration (declare+publish)", N, node_reg);

  // Add edges in a topological producer<consumer pattern.
  t0 = Clock::now();
  std::size_t added = 0;
  for (std::size_t i = 0; i < E; ++i) {
    const std::uint64_t prod = 1 + (i % (N - 1));          // 1 .. N-1
    const std::uint64_t span = N - prod;
    const std::uint64_t cons = prod + 1 + ((i * 37) % span);  // prod+1 .. N
    Edge ed;
    ed.id = DependencyEdgeId(i + 1);
    ed.generation = DependencyEdgeGeneration(1);
    ed.kind = EdgeKind::REQUIRES_READY;
    ed.producer_id = DependencyNodeId(prod);
    ed.consumer_id = DependencyNodeId(cons);
    ed.predicate.required_readiness = ReadinessState::READY;
    ed.predicate.is_required = true;
    if (g.add_edge(ed, src, boot, epoch) == Outcome::ACCEPTED) ++added;
  }
  const double edge_reg = ms(t0, Clock::now());
  time_op("edge registration", added, edge_reg);

  // Readiness evaluation over the whole graph.
  t0 = Clock::now();
  (void)g.recompute_all_readiness();
  const double recomp = ms(t0, Clock::now());
  time_op("readiness evaluation (all nodes)", N, recomp);

  // Blocker query for a mid node.
  t0 = Clock::now();
  const std::uint64_t mid = N / 2;
  for (std::size_t i = 0; i < 1000; ++i) (void)g.blockers(DependencyNodeId(mid));
  const double blocker = ms(t0, Clock::now());
  time_op("blocker query", 1000, blocker);

  // Descendant and ancestor traversal.
  t0 = Clock::now();
  for (std::size_t i = 0; i < 200; ++i) {
    (void)g.dependents(DependencyNodeId(1), true);
    (void)g.prerequisites(DependencyNodeId(mid), true);
  }
  const double trav = ms(t0, Clock::now());
  time_op("descendant+ancestor traversal", 200, trav);

  // Invalidation propagation (recursive) and recovery.
  InvalidationPolicy pol;
  pol.mode = InvalidationMode::RECURSIVE;
  t0 = Clock::now();
  (void)g.invalidate_node(DependencyNodeId(1), InvalidationReason::EXPLICIT, pol, src, boot, epoch);
  const double inval = ms(t0, Clock::now());
  time_op("recursive invalidation propagation", 1, inval);
  t0 = Clock::now();
  (void)g.recompute_all_readiness();
  const double recover = ms(t0, Clock::now());
  time_op("recovery/recompute after invalidation", N, recover);
  (void)g.mark_recovered(DependencyNodeId(1), src, boot, epoch);

  // Generation supersession.
  t0 = Clock::now();
  for (std::size_t i = 0; i < 1000; ++i) {
    const std::uint64_t nid = 1 + (i % N);
    (void)g.publish_generation(DependencyNodeId(nid), DependencyNodeGeneration(2), src, boot, epoch);
  }
  time_op("generation supersession", 1000, ms(t0, Clock::now()));

  // Persistence save / recover.
  t0 = Clock::now();
  auto bytes = g.serialize();
  const double save = ms(t0, Clock::now());
  time_op("persistence save", g.node_count(), save);
  t0 = Clock::now();
  Graph g2;
  (void)Persistence::load(*bytes, &g2, nullptr);
  const double load = ms(t0, Clock::now());
  time_op("persistence recover", g2.node_count(), load);

  // Concurrent read-heavy queries.
  {
    std::atomic<bool> go{false};
    std::vector<std::thread> th;
    const int nthreads = 4;
    for (int t = 0; t < nthreads; ++t) {
      th.emplace_back([&] {
        while (!go.load()) std::this_thread::yield();
        for (std::size_t i = 0; i < 2000; ++i) {
          const std::uint64_t nid = 1 + (i % N);
          (void)g.readiness(DependencyNodeId(nid));
          (void)g.blockers(DependencyNodeId(nid));
        }
      });
    }
    t0 = Clock::now();
    go.store(true);
    for (auto& t : th) t.join();
    time_op("concurrent read-heavy queries (4 threads)", 4 * 2000, ms(t0, Clock::now()));
  }
  std::printf("\n");
}

}  // namespace

int main() {
  std::printf("Dependency Fabric benchmarks (Release, MSVC /W4 /WX)\n\n");
  run_case({1000, 10000});
  run_case({10000, 20000});
  // Larger graph (edges ~5x nodes) where practical.
  run_case({20000, 60000});
  return 0;
}