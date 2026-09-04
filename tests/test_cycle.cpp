// dependency_fabric::test — cycle detection, duplicate edges, stale generations.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "test_util.hpp"

using namespace df_test;
using namespace dependency_fabric;

int main() {
  tf::Fixture f;

  // Build A(1)->B(2)->C(3): B depends on A, C depends on B.
  CHECK_EQ(f.declare(1, NodeKind::MODEL, "a"), Outcome::ACCEPTED);
  CHECK_EQ(f.declare(2, NodeKind::ARTIFACT, "b"), Outcome::ACCEPTED);
  CHECK_EQ(f.declare(3, NodeKind::WORKLOAD_PHASE, "c"), Outcome::ACCEPTED);
  CHECK_EQ(f.pub(1, 1), Outcome::ACCEPTED);
  CHECK_EQ(f.pub(2, 1), Outcome::ACCEPTED);
  CHECK_EQ(f.add(10, 1, 2, 1), Outcome::ACCEPTED);
  CHECK_EQ(f.add(11, 2, 3, 1), Outcome::ACCEPTED);

  // self-cycle
  CHECK_EQ(f.add(12, 1, 1, 1), Outcome::REJECT_CYCLE);
  CHECK_EQ(f.add(13, 2, 2, 1), Outcome::REJECT_CYCLE);

  // Multi-node cycle: add C->A (A depends on C) creates A->B->C->A.
  CHECK_EQ(f.add(14, 3, 1, 1), Outcome::REJECT_CYCLE);
  // And A->C is fine (no back-path from C to A).
  CHECK_EQ(f.add(15, 1, 3, 1), Outcome::ACCEPTED);

  // Missing endpoint.
  CHECK_EQ(f.add(16, 999, 1, 1), Outcome::REJECT_UNKNOWN_DEPENDENCY);
  CHECK_EQ(f.add(17, 1, 999, 1), Outcome::REJECT_UNKNOWN_DEPENDENCY);

  // Duplicate edge (same producer/consumer/kind).
  CHECK_EQ(f.add(18, 1, 3, 1), Outcome::REJECT_DUPLICATE_ID);

  // Duplicate node id.
  CHECK_EQ(f.declare(1, NodeKind::MODEL, "dup"), Outcome::REJECT_DUPLICATE_ID);

  // Stale generation: publish a lower/equal model generation is rejected.
  CHECK_EQ(f.pub(1, 1), Outcome::REJECT_DUPLICATE_ID);
  CHECK_EQ(f.pub(1, 0), Outcome::REJECT_INVALID_TRANSITION);
  // Monotonic advance is fine.
  CHECK_EQ(f.pub(1, 2), Outcome::ACCEPTED);
  // Generation regression via supersede rejected.
  CHECK_EQ(f.g.supersede_generation(DependencyNodeId(1), DependencyNodeGeneration(2),
                                    DependencyNodeGeneration(1), f.src, f.boot, f.epoch),
           Outcome::REJECT_STALE_NODE_GENERATION);

  // The graph stays acyclic throughout.
  CHECK(f.g.acyclic());
  CHECK_EQ(f.g.self_loop_edges().size(), 0u);

  // Edge to a retired node is still a valid target (node exists).
  CHECK_EQ(f.g.retire_node(DependencyNodeId(3), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
  CHECK_EQ(f.ready(3), ReadinessState::RETIRED);

  return summary("cycle");
}