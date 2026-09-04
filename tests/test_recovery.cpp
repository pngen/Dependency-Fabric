// dependency_fabric::test — recovery planning, cascading recovery, closure queries.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "test_util.hpp"

using namespace df_test;
using namespace dependency_fabric;

namespace {
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
}  // namespace

int main() {
  // --- cascading recovery: model gen change propagates through the chain ----
  {
    tf::Fixture f;
    // model(1) -> adapter(2) -> execution_graph(3) -> kernel(4) -> phase(5)
    f.declare(1, NodeKind::MODEL, "model");
    f.declare(2, NodeKind::ADAPTER, "adapter");
    f.declare(3, NodeKind::EXECUTION_GRAPH, "graph");
    f.declare(4, NodeKind::KERNEL, "kernel");
    f.declare(5, NodeKind::WORKLOAD_PHASE, "phase");
    for (int i = 1; i <= 5; ++i) { f.pub(i, 1); CHECK_EQ(f.ready(i), ReadinessState::READY); }
    CHECK_EQ(f.g.add_edge(tf::Fixture::pin_edge(100, 1, 2, 1), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(tf::Fixture::pin_edge(101, 2, 3, 1), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(tf::Fixture::pin_edge(102, 3, 4, 1), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(tf::Fixture::pin_edge(103, 4, 5, 1), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    f.g.recompute_all_readiness();
    CHECK_EQ(f.ready(5), ReadinessState::READY);

    // Model gen advances to 2: the whole downstream chain goes stale/blocked.
    CHECK_EQ(f.pub(1, 2), Outcome::ACCEPTED);
    CHECK_EQ(f.ready(2), ReadinessState::STALE);
    CHECK_EQ(f.ready(3), ReadinessState::STALE);
    CHECK_EQ(f.ready(4), ReadinessState::STALE);
    CHECK_EQ(f.ready(5), ReadinessState::BLOCKED);  // depends on kernel which is stale

    // Recovery plan identifies rebuild/revalidation obligations, ordered by
    // topology (adapter first, then graph, kernel, phase).
    RecoveryPlan plan = f.g.recovery_plan(DependencyNodeId(1));
    CHECK(!plan.intents.empty());
    // The recovery intents should mention the affected nodes (2..5).
    bool saw_recompute = false;
    for (const auto& intent : plan.intents) {
      CHECK(intent.node.value() >= 2u && intent.node.value() <= 5u);
      if (intent.action == RecoveryAction::RECOMPUTATION || intent.action == RecoveryAction::RESTART) saw_recompute = true;
      CHECK(!intent.owner_runtime.empty());
    }
    CHECK(saw_recompute);

    // recompute_frontier returns the top of the affected region (adapter=2).
    auto frontier = f.g.recompute_frontier(DependencyNodeId(1));
    CHECK(!frontier.empty());
    CHECK(frontier[0].value() == 2u);
  }

  // --- explicit invalidation recovery --------------------------------------
  {
    tf::Fixture f;
    f.declare(1, NodeKind::MODEL, "a");
    f.declare(2, NodeKind::ARTIFACT, "b");
    f.declare(3, NodeKind::WORKLOAD_PHASE, "c");
    f.pub(1, 1); f.pub(2, 1);
    CHECK_EQ(f.g.add_edge(ready_edge(100, 1, 2), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(ready_edge(101, 2, 3), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    f.g.recompute_all_readiness();
    CHECK_EQ(f.ready(3), ReadinessState::READY);

    InvalidationPolicy pol; pol.mode = InvalidationMode::RECURSIVE;
    CHECK_EQ(f.g.invalidate_node(DependencyNodeId(2), InvalidationReason::INTEGRITY_FAILED, pol, f.src, f.boot, f.epoch),
             Outcome::ACCEPTED);
    CHECK_EQ(f.ready(3), ReadinessState::BLOCKED);

    Explanation ex = f.g.explain(DependencyNodeId(3));
    CHECK(ex.state == ReadinessState::BLOCKED);
    CHECK(!ex.upstream_invalidation_notes.empty());
    CHECK(!ex.blockers.empty());

    // After recovery of node 2, node 3 is READY again.
    CHECK_EQ(f.g.mark_recovered(DependencyNodeId(2), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.publish_readiness(DependencyNodeId(2), ReadinessState::READY, f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.ready(2), ReadinessState::READY);
    CHECK_EQ(f.ready(3), ReadinessState::READY);
  }

  // --- affected closure / recovery set -------------------------------------
  {
    tf::Fixture f;
    // 1->2->3 and 1->4 (branch)
    f.declare(1, NodeKind::MODEL, "a");
    f.declare(2, NodeKind::ARTIFACT, "b");
    f.declare(3, NodeKind::ARTIFACT, "c");
    f.declare(4, NodeKind::ARTIFACT, "d");
    f.pub(1, 1); f.pub(2, 1); f.pub(3, 1); f.pub(4, 1);
    CHECK_EQ(f.g.add_edge(ready_edge(100, 1, 2), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(ready_edge(101, 2, 3), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(ready_edge(102, 1, 4), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    // A sibling 5 independent of 1 (not affected).
    f.declare(5, NodeKind::ARTIFACT, "e"); f.pub(5, 1);

    InvalidationPolicy pol; pol.mode = InvalidationMode::RECURSIVE;
    CHECK_EQ(f.g.invalidate_node(DependencyNodeId(1), InvalidationReason::UPSTREAM_GONE, pol, f.src, f.boot, f.epoch),
             Outcome::ACCEPTED);

    AffectedClosure cl = f.g.affected_closure(DependencyNodeId(1), 0);
    // affected: 1,2,3,4 (not 5).
    CHECK(cl.nodes.size() == 4u);
    CHECK(cl.edges.size() == 3u);
    // recompute frontier: the boundary nodes (3 and 4) are leaves.
    for (auto n : cl.nodes) CHECK(n.value() != 5u);
  }

  return summary("recovery");
}