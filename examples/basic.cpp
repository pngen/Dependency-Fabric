// dependency_fabric::example — basic usage of the runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include "dependency_fabric/df.hpp"

using namespace dependency_fabric;

int main() {
  const SourceId src(1);
  const SourceBootId boot(1);
  const CoordinatorEpoch epoch(1);
  Graph g;
  g.register_source(src, boot, epoch);

  // Declare a model, an artifact, and a workload phase.
  g.declare_node(DependencyNodeId(1), NodeKind::MODEL, "llama-70b", src, boot, epoch);
  g.declare_node(DependencyNodeId(2), NodeKind::ARTIFACT, "adapter-v1", src, boot, epoch);
  g.declare_node(DependencyNodeId(3), NodeKind::WORKLOAD_PHASE, "inference", src, boot, epoch);

  // Publish content generations.
  g.publish_generation(DependencyNodeId(1), DependencyNodeGeneration(1), src, boot, epoch);
  g.publish_generation(DependencyNodeId(2), DependencyNodeGeneration(1), src, boot, epoch);

  // The artifact is built against model gen1; the inference phase needs the
  // artifact READY.
  Edge e_ma;
  e_ma.id = DependencyEdgeId(1); e_ma.generation = DependencyEdgeGeneration(1);
  e_ma.kind = EdgeKind::REQUIRES_GENERATION;
  e_ma.producer_id = DependencyNodeId(1); e_ma.consumer_id = DependencyNodeId(2);
  e_ma.predicate.generation_kind = GenerationPredicateKind::EXACT;
  e_ma.predicate.exact_generation = DependencyNodeGeneration(1);
  e_ma.predicate.required_readiness = ReadinessState::READY;
  e_ma.predicate.is_required = true;
  g.add_edge(e_ma, src, boot, epoch);

  Edge e_ai;
  e_ai.id = DependencyEdgeId(2); e_ai.generation = DependencyEdgeGeneration(1);
  e_ai.kind = EdgeKind::REQUIRES_READY;
  e_ai.producer_id = DependencyNodeId(2); e_ai.consumer_id = DependencyNodeId(3);
  e_ai.predicate.required_readiness = ReadinessState::READY;
  e_ai.predicate.is_required = true;
  g.add_edge(e_ai, src, boot, epoch);

  g.recompute_all_readiness();
  std::printf("phase readiness: %s\n", std::string(name_of(g.readiness(DependencyNodeId(3)))).c_str());

  // The model advances: the artifact becomes stale and the phase blocks.
  g.publish_generation(DependencyNodeId(1), DependencyNodeGeneration(2), src, boot, epoch);
  std::printf("after model gen2 -> artifact=%s phase=%s\n",
              std::string(name_of(g.readiness(DependencyNodeId(2)))).c_str(),
              std::string(name_of(g.readiness(DependencyNodeId(3)))).c_str());

  // Ask why the phase is blocked.
  auto ex = g.explain(DependencyNodeId(3));
  std::printf("why: %s\n", ex.summary.c_str());
  for (const auto& b : ex.blockers) std::printf("  blocker: %s\n", b.reason.c_str());
  return 0;
}
