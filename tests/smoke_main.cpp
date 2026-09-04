// dependency_fabric::smoke — first end-to-end smoke of the core graph.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dependency_fabric/df.hpp"
#include "test_framework.hpp"

using namespace dependency_fabric;

static const SourceId kSource(1);
static const SourceBootId kBoot(1);
static const CoordinatorEpoch kEpoch(1);

static Edge req_edge(DependencyEdgeId eid, DependencyNodeId producer, DependencyNodeId consumer,
                     DependencyNodeGeneration pinned) {
  Edge e;
  e.id = eid;
  e.generation = DependencyEdgeGeneration(1);
  e.kind = EdgeKind::REQUIRES_GENERATION;
  e.producer_id = producer;
  e.consumer_id = consumer;
  e.predicate.generation_kind = GenerationPredicateKind::EXACT;
  e.predicate.exact_generation = pinned;
  e.predicate.is_required = true;
  return e;
}

int main() {
  Graph g;
  CHECK_EQ(g.register_source(kSource, kBoot, kEpoch), Outcome::ACCEPTED);

  // Nodes: model (1), artifact (2), consumer phase (3).
  CHECK_EQ(g.declare_node(DependencyNodeId(1), NodeKind::MODEL, "model", kSource, kBoot, kEpoch), Outcome::ACCEPTED);
  CHECK_EQ(g.declare_node(DependencyNodeId(2), NodeKind::ARTIFACT, "artifact", kSource, kBoot, kEpoch), Outcome::ACCEPTED);
  CHECK_EQ(g.declare_node(DependencyNodeId(3), NodeKind::WORKLOAD_PHASE, "phase", kSource, kBoot, kEpoch), Outcome::ACCEPTED);

  CHECK_EQ(g.publish_generation(DependencyNodeId(1), DependencyNodeGeneration(1), kSource, kBoot, kEpoch), Outcome::ACCEPTED);
  CHECK_EQ(g.publish_generation(DependencyNodeId(2), DependencyNodeGeneration(1), kSource, kBoot, kEpoch), Outcome::ACCEPTED);

  // Artifact (2) depends on Model (1) gen1; Consumer (3) depends on Artifact (2) gen1.
  CHECK_EQ(g.add_edge(req_edge(DependencyEdgeId(1), DependencyNodeId(1), DependencyNodeId(2), DependencyNodeGeneration(1)), kSource, kBoot, kEpoch), Outcome::ACCEPTED);
  CHECK_EQ(g.add_edge(req_edge(DependencyEdgeId(2), DependencyNodeId(2), DependencyNodeId(3), DependencyNodeGeneration(1)), kSource, kBoot, kEpoch), Outcome::ACCEPTED);

  CHECK_EQ(g.recompute_all_readiness(), Outcome::ACCEPTED);
  CHECK_EQ(g.readiness(DependencyNodeId(1)), ReadinessState::READY);
  CHECK_EQ(g.readiness(DependencyNodeId(2)), ReadinessState::READY);
  CHECK_EQ(g.readiness(DependencyNodeId(3)), ReadinessState::READY);

  // Model generation advances to 2: artifact (bound EXACT gen1) becomes STALE,
  // consumer becomes BLOCKED.
  CHECK_EQ(g.publish_generation(DependencyNodeId(1), DependencyNodeGeneration(2), kSource, kBoot, kEpoch), Outcome::ACCEPTED);
  CHECK_EQ(g.readiness(DependencyNodeId(2)), ReadinessState::STALE);
  CHECK_EQ(g.readiness(DependencyNodeId(3)), ReadinessState::BLOCKED);

  // Rebuild: artifact is re-bound to Model gen2 and publishes gen2.
  CHECK_EQ(g.remove_edge(DependencyEdgeId(1), kSource, kBoot, kEpoch), Outcome::ACCEPTED);
  CHECK_EQ(g.add_edge(req_edge(DependencyEdgeId(5), DependencyNodeId(1), DependencyNodeId(2), DependencyNodeGeneration(2)), kSource, kBoot, kEpoch), Outcome::ACCEPTED);
  CHECK_EQ(g.publish_generation(DependencyNodeId(2), DependencyNodeGeneration(2), kSource, kBoot, kEpoch), Outcome::ACCEPTED);
  CHECK_EQ(g.readiness(DependencyNodeId(2)), ReadinessState::READY);

  // Consumer re-bound to Artifact gen2 -> READY.
  CHECK_EQ(g.remove_edge(DependencyEdgeId(2), kSource, kBoot, kEpoch), Outcome::ACCEPTED);
  CHECK_EQ(g.add_edge(req_edge(DependencyEdgeId(6), DependencyNodeId(2), DependencyNodeId(3), DependencyNodeGeneration(2)), kSource, kBoot, kEpoch), Outcome::ACCEPTED);
  CHECK_EQ(g.readiness(DependencyNodeId(3)), ReadinessState::READY);

  // Persistence round trip: durable topology reconstructs; dynamic readiness
  // is conservative (REVALIDATION_REQUIRED) until evidence is republished.
  auto bytes = g.serialize();
  CHECK(bytes.has_value());
  Graph g2;
  CHECK_EQ(Persistence::load(*bytes, &g2, nullptr), Outcome::ACCEPTED);
  CHECK_EQ(g2.node_count(), g.node_count());
  CHECK_EQ(g2.edge_count(), g.edge_count());
  CHECK_EQ(g2.readiness(DependencyNodeId(2)), ReadinessState::REVALIDATION_REQUIRED);
  CHECK_EQ(g2.readiness(DependencyNodeId(3)), ReadinessState::REVALIDATION_REQUIRED);
  // Republish current evidence -> READY (bottom-up: Model first).
  CHECK_EQ(g2.publish_readiness(DependencyNodeId(1), ReadinessState::READY, kSource, kBoot, kEpoch), Outcome::ACCEPTED);
  CHECK_EQ(g2.publish_readiness(DependencyNodeId(2), ReadinessState::READY, kSource, kBoot, kEpoch), Outcome::ACCEPTED);
  CHECK_EQ(g2.publish_readiness(DependencyNodeId(3), ReadinessState::READY, kSource, kBoot, kEpoch), Outcome::ACCEPTED);
  CHECK_EQ(g2.readiness(DependencyNodeId(2)), ReadinessState::READY);
  CHECK_EQ(g2.readiness(DependencyNodeId(3)), ReadinessState::READY);

  // Cycle rejection.
  Edge cyc = req_edge(DependencyEdgeId(3), DependencyNodeId(2), DependencyNodeId(1), DependencyNodeGeneration(1));
  CHECK_EQ(g.add_edge(cyc, kSource, kBoot, kEpoch), Outcome::REJECT_CYCLE);

  // Self cycle.
  Edge selfc = req_edge(DependencyEdgeId(4), DependencyNodeId(1), DependencyNodeId(1), DependencyNodeGeneration(1));
  CHECK_EQ(g.add_edge(selfc, kSource, kBoot, kEpoch), Outcome::REJECT_CYCLE);

  // Duplicate edge rejection.
  Edge dup = req_edge(DependencyEdgeId(7), DependencyNodeId(1), DependencyNodeId(2), DependencyNodeGeneration(2));
  CHECK_EQ(g.add_edge(dup, kSource, kBoot, kEpoch), Outcome::REJECT_DUPLICATE_ID);

  // Stale generation rejection (publishing a lower/equal generation).
  CHECK_EQ(g.publish_generation(DependencyNodeId(1), DependencyNodeGeneration(1), kSource, kBoot, kEpoch), Outcome::REJECT_STALE_NODE_GENERATION);

  return df_test::summary("smoke");
}