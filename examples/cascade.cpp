// dependency_fabric::example — cascading invalidation and recovery planning.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include "dependency_fabric/df.hpp"

using namespace dependency_fabric;

Edge ready_edge(DependencyEdgeId id, DependencyNodeId prod, DependencyNodeId cons) {
  Edge e;
  e.id = id; e.generation = DependencyEdgeGeneration(1); e.kind = EdgeKind::REQUIRES_READY;
  e.producer_id = prod; e.consumer_id = cons;
  e.predicate.required_readiness = ReadinessState::READY; e.predicate.is_required = true;
  return e;
}

int main() {
  Graph g;
  const SourceId src(1); const SourceBootId boot(1); const CoordinatorEpoch epoch(1);
  g.register_source(src, boot, epoch);

  // model(1) -> adapter(2) -> execution_graph(3) -> kernel(4) -> phase(5)
  const char* names[] = {"model", "adapter", "execution_graph", "kernel", "phase"};
  for (std::uint64_t i = 1; i <= 5; ++i) {
    g.declare_node(DependencyNodeId(i), NodeKind::MODEL, names[i - 1], src, boot, epoch, true);
    g.publish_generation(DependencyNodeId(i), DependencyNodeGeneration(1), src, boot, epoch);
  }
  for (std::uint64_t i = 1; i < 5; ++i) g.add_edge(ready_edge(DependencyEdgeId(i), DependencyNodeId(i), DependencyNodeId(i + 1)), src, boot, epoch);
  g.recompute_all_readiness();
  std::printf("phase=%s\n", std::string(name_of(g.readiness(DependencyNodeId(5)))).c_str());

  // Invalidate the adapter: the downstream cascade blocks.
  InvalidationPolicy pol; pol.mode = InvalidationMode::RECURSIVE;
  g.invalidate_node(DependencyNodeId(2), InvalidationReason::COMPATIBILITY_CHANGED, pol, src, boot, epoch);
  std::printf("after adapter invalidation -> adapter=%s graph=%s kernel=%s phase=%s\n",
              std::string(name_of(g.readiness(DependencyNodeId(2)))).c_str(),
              std::string(name_of(g.readiness(DependencyNodeId(3)))).c_str(),
              std::string(name_of(g.readiness(DependencyNodeId(4)))).c_str(),
              std::string(name_of(g.readiness(DependencyNodeId(5)))).c_str());

  // Recovery plan: which owners must act, in dependency order.
  auto plan = g.recovery_plan(DependencyNodeId(2));
  for (const auto& intent : plan.intents) {
    std::printf("recover node %llu (%s) -> %s by %s\n",
                static_cast<unsigned long long>(intent.node.value()),
                std::string(name_of(intent.kind)).c_str(),
                std::string(name_of(intent.action)).c_str(),
                intent.owner_runtime.c_str());
  }
  return 0;
}