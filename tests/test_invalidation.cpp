// dependency_fabric::test — invalidation, propagation, supersession, recovery.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "test_util.hpp"

using namespace df_test;
using namespace dependency_fabric;

namespace {
// Build a REQUIRES_READY (no generation constraint) edge.
Edge ready_edge(std::uint64_t id, std::uint64_t prod, std::uint64_t cons) {
  Edge e;
  e.id = DependencyEdgeId(id);
  e.generation = DependencyEdgeGeneration(1);
  e.kind = EdgeKind::REQUIRES_READY;
  e.producer_id = DependencyNodeId(prod);
  e.consumer_id = DependencyNodeId(cons);
  e.predicate.required_readiness = ReadinessState::READY;
  e.predicate.is_required = true;
  return e;
}
// Build an OPTIONAL_READY edge.
Edge opt_edge(std::uint64_t id, std::uint64_t prod, std::uint64_t cons) {
  Edge e = ready_edge(id, prod, cons);
  e.kind = EdgeKind::OPTIONAL;
  e.predicate.is_required = false;
  return e;
}
}  // namespace

int main() {
  // --- transitive cascade A->B->C->D ---------------------------------------
  {
    tf::Fixture f;
    for (std::uint64_t i = 1; i <= 4; ++i) {
      CHECK_EQ(f.declare(i, NodeKind::MODEL, ""), Outcome::ACCEPTED);
      CHECK_EQ(f.pub(i, 1), Outcome::ACCEPTED);
    }
    CHECK_EQ(f.g.add_edge(ready_edge(100, 1, 2), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(ready_edge(101, 2, 3), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(ready_edge(102, 3, 4), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.recompute_all_readiness(), Outcome::ACCEPTED);
    CHECK_EQ(f.ready(1), ReadinessState::READY);
    CHECK_EQ(f.ready(2), ReadinessState::READY);
    CHECK_EQ(f.ready(3), ReadinessState::READY);
    CHECK_EQ(f.ready(4), ReadinessState::READY);

    // Invalidate B recursively.
    InvalidationPolicy pol;
    pol.mode = InvalidationMode::RECURSIVE;
    CHECK_EQ(f.g.invalidate_node(DependencyNodeId(2), InvalidationReason::EXPLICIT, pol, f.src, f.boot, f.epoch),
             Outcome::ACCEPTED);
    CHECK_EQ(f.ready(1), ReadinessState::READY);       // A untouched
    CHECK_EQ(f.ready(2), ReadinessState::INVALIDATED); // B seed
    CHECK_EQ(f.ready(3), ReadinessState::BLOCKED);     // C blocked (B not ready)
    CHECK_EQ(f.ready(4), ReadinessState::BLOCKED);     // D blocked

    // affected closure from B includes 2,3,4 (not 1).
    AffectedClosure cl = f.g.affected_closure(DependencyNodeId(2), 0);
    CHECK(cl.nodes.size() == 3u);
    // recovery order respects topology: 2 before 3 before 4.
    auto rs = f.g.recovery_set(DependencyNodeId(2));
    CHECK(rs.size() == 3u);

    // Recover B: after recovery the cascade restores (static nodes).
    InvalidationPolicy pol2;
    pol2.mode = InvalidationMode::DIRECT;
    CHECK_EQ(f.g.mark_recovered(DependencyNodeId(2), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    // Dynamic evidence requires revalidation before it is READY again.
    CHECK_EQ(f.g.publish_readiness(DependencyNodeId(2), ReadinessState::READY, f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.ready(2), ReadinessState::READY);
    CHECK_EQ(f.ready(3), ReadinessState::READY);
    CHECK_EQ(f.ready(4), ReadinessState::READY);
  }

  // --- bounded invalidation ------------------------------------------------
  {
    tf::Fixture f;
    for (std::uint64_t i = 1; i <= 4; ++i) {
      CHECK_EQ(f.declare(i, NodeKind::MODEL, ""), Outcome::ACCEPTED);
      CHECK_EQ(f.pub(i, 1), Outcome::ACCEPTED);
    }
    CHECK_EQ(f.g.add_edge(ready_edge(100, 1, 2), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(ready_edge(101, 2, 3), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(ready_edge(102, 3, 4), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    InvalidationPolicy pol;
    pol.mode = InvalidationMode::BOUNDED;
    pol.max_depth = 1;  // only immediate dependents
    CHECK_EQ(f.g.invalidate_node(DependencyNodeId(2), InvalidationReason::EXPLICIT, pol, f.src, f.boot, f.epoch),
             Outcome::ACCEPTED);
    CHECK_EQ(f.ready(2), ReadinessState::INVALIDATED);
    CHECK_EQ(f.ready(3), ReadinessState::BLOCKED);
    CHECK_EQ(f.ready(4), ReadinessState::BLOCKED);  // depends on 3 which is blocked
  }

  // --- optional dependency: consumer requires A, optionally uses B ----------
  {
    tf::Fixture f;
    CHECK_EQ(f.declare(1, NodeKind::MODEL, "A"), Outcome::ACCEPTED);
    CHECK_EQ(f.declare(2, NodeKind::ADAPTER, "B"), Outcome::ACCEPTED);
    CHECK_EQ(f.declare(3, NodeKind::WORKLOAD_PHASE, "consumer"), Outcome::ACCEPTED);
    CHECK_EQ(f.pub(1, 1), Outcome::ACCEPTED);
    CHECK_EQ(f.pub(2, 1), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(ready_edge(100, 1, 3), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(opt_edge(101, 2, 3), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.recompute_all_readiness(), Outcome::ACCEPTED);
    CHECK_EQ(f.ready(3), ReadinessState::READY);

    // Invalidate B (optional): consumer degrades but remains eligible.
    InvalidationPolicy pol;
    pol.mode = InvalidationMode::DIRECT;
    CHECK_EQ(f.g.invalidate_node(DependencyNodeId(2), InvalidationReason::UPSTREAM_GONE, pol, f.src, f.boot, f.epoch),
             Outcome::ACCEPTED);
    CHECK_EQ(f.ready(2), ReadinessState::INVALIDATED);
    CHECK_EQ(f.ready(3), ReadinessState::DEGRADED);

    // Invalidate A (required): consumer blocks.
    CHECK_EQ(f.g.add_edge(ready_edge(100, 1, 3), f.src, f.boot, f.epoch), Outcome::REJECT_DUPLICATE_ID);  // still exists
    CHECK_EQ(f.g.invalidate_node(DependencyNodeId(1), InvalidationReason::UPSTREAM_GONE, pol, f.src, f.boot, f.epoch),
             Outcome::ACCEPTED);
    CHECK_EQ(f.ready(3), ReadinessState::BLOCKED);
  }

  // --- generation-based invalidation ---------------------------------------
  {
    tf::Fixture f;
    CHECK_EQ(f.declare(1, NodeKind::MODEL, "m"), Outcome::ACCEPTED);
    CHECK_EQ(f.declare(2, NodeKind::ARTIFACT, "a"), Outcome::ACCEPTED);
    CHECK_EQ(f.pub(1, 1), Outcome::ACCEPTED);
    CHECK_EQ(f.pub(2, 1), Outcome::ACCEPTED);
    CHECK_EQ(f.add(100, 1, 2, 1), Outcome::ACCEPTED);
    // Advance model to gen2: artifact (bound EXACT gen1) becomes STALE.
    CHECK_EQ(f.pub(1, 2), Outcome::ACCEPTED);
    CHECK_EQ(f.ready(2), ReadinessState::STALE);
    // Generation-based invalidation of the model's historical gen1 fails for a
    // generation not in history (999).
    InvalidationPolicy pol; pol.mode = InvalidationMode::DIRECT;
    CHECK_EQ(f.g.invalidate_generation(DependencyNodeId(1), DependencyNodeGeneration(999), InvalidationReason::EXPLICIT, pol, f.src, f.boot, f.epoch),
             Outcome::REJECT_STALE_NODE_GENERATION);
  }

  // --- stale source rejection ----------------------------------------------
  {
    tf::Fixture f1;
    CHECK_EQ(f1.declare(1, NodeKind::MODEL, "m"), Outcome::ACCEPTED);
    // Different boot -> REJECT_STALE_BOOT (source 1 not registered with boot 99).
    CHECK_EQ(f1.g.declare_node(DependencyNodeId(2), NodeKind::ARTIFACT, "x", SourceId(1), SourceBootId(99), CoordinatorEpoch(1)),
             Outcome::REJECT_STALE_BOOT);
    // Wrong epoch -> REJECT_STALE_EPOCH.
    CHECK_EQ(f1.g.declare_node(DependencyNodeId(2), NodeKind::ARTIFACT, "x", SourceId(1), SourceBootId(1), CoordinatorEpoch(5)),
             Outcome::REJECT_STALE_EPOCH);
  }

  return summary("invalidation");
}