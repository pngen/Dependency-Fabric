// dependency_fabric::test — dependency set operators.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "test_util.hpp"

using namespace df_test;
using namespace dependency_fabric;

namespace {
Edge ready_edge(std::uint64_t id, std::uint64_t prod, std::uint64_t cons, bool required = true) {
  Edge e;
  e.id = DependencyEdgeId(id); e.generation = DependencyEdgeGeneration(1); e.kind = EdgeKind::REQUIRES_READY;
  e.producer_id = DependencyNodeId(prod); e.consumer_id = DependencyNodeId(cons);
  e.predicate.required_readiness = ReadinessState::READY; e.predicate.is_required = required;
  return e;
}

DependencySet make_set(DependencySetId id, std::uint64_t owner, SetOperator op, std::uint32_t n, std::vector<DependencyEdgeId> members) {
  DependencySet s;
  s.id = id; s.generation = DependencySetGeneration(1); s.owner = DependencyNodeId(owner);
  s.op = op; s.at_least_n = n; s.members = std::move(members);
  return s;
}

void add_sources(int n, tf::Fixture& f) {
  for (int i = 1; i <= n; ++i) { f.declare(i, NodeKind::MODEL, ""); f.pub(i, 1); }
  f.declare(n + 1, NodeKind::WORKLOAD_PHASE, "consumer");
}
}  // namespace

int main() {
  // ANY_OF: consumer needs at least one of {1,2}; 1 READY, 2 blocked -> READY.
  {
    tf::Fixture f;
    add_sources(2, f);
    CHECK_EQ(f.g.add_edge(ready_edge(1, 1, 3), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(ready_edge(2, 2, 3), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    InvalidationPolicy pol; pol.mode = InvalidationMode::DIRECT;
    CHECK_EQ(f.g.invalidate_node(DependencyNodeId(2), InvalidationReason::UPSTREAM_GONE, pol, f.src, f.boot, f.epoch),
             Outcome::ACCEPTED);
    // Node 2 invalidated; node 1 READY. ANY_OF -> satisfied.
    CHECK_EQ(f.g.add_dependency_set(make_set(DependencySetId(1), 3, SetOperator::ANY_OF, 1,
                                             {DependencyEdgeId(1), DependencyEdgeId(2)}),
                                    f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    f.g.recompute_all_readiness();
    CHECK_EQ(f.ready(3), ReadinessState::READY);
  }

  // ALL_OF: consumer needs both {1,2}; 2 invalid -> BLOCKED.
  {
    tf::Fixture f;
    add_sources(2, f);
    CHECK_EQ(f.g.add_edge(ready_edge(1, 1, 3), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(ready_edge(2, 2, 3), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    InvalidationPolicy pol; pol.mode = InvalidationMode::DIRECT;
    CHECK_EQ(f.g.invalidate_node(DependencyNodeId(2), InvalidationReason::UPSTREAM_GONE, pol, f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_dependency_set(make_set(DependencySetId(1), 3, SetOperator::ALL_OF, 1,
                                            {DependencyEdgeId(1), DependencyEdgeId(2)}),
                                    f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    f.g.recompute_all_readiness();
    CHECK_EQ(f.ready(3), ReadinessState::BLOCKED);
  }

  // AT_LEAST_N(2): consumer needs >=2 of {1,2,3}; 2 invalid, 1+3 READY -> READY.
  {
    tf::Fixture f;
    add_sources(3, f);
    CHECK_EQ(f.g.add_edge(ready_edge(1, 1, 4), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(ready_edge(2, 2, 4), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(ready_edge(3, 3, 4), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    InvalidationPolicy pol; pol.mode = InvalidationMode::DIRECT;
    CHECK_EQ(f.g.invalidate_node(DependencyNodeId(2), InvalidationReason::UPSTREAM_GONE, pol, f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_dependency_set(make_set(DependencySetId(1), 4, SetOperator::AT_LEAST_N, 2,
                                            {DependencyEdgeId(1), DependencyEdgeId(2), DependencyEdgeId(3)}),
                                    f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    f.g.recompute_all_readiness();
    CHECK_EQ(f.ready(4), ReadinessState::READY);
  }

  // OPTIONAL_GROUP: never hard-blocks; both invalid -> DEGRADED.
  {
    tf::Fixture f;
    add_sources(2, f);
    CHECK_EQ(f.g.add_edge(ready_edge(1, 1, 3), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_edge(ready_edge(2, 2, 3), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    InvalidationPolicy pol; pol.mode = InvalidationMode::DIRECT;
    CHECK_EQ(f.g.invalidate_node(DependencyNodeId(1), InvalidationReason::UPSTREAM_GONE, pol, f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.invalidate_node(DependencyNodeId(2), InvalidationReason::UPSTREAM_GONE, pol, f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.add_dependency_set(make_set(DependencySetId(1), 3, SetOperator::OPTIONAL_GROUP, 0,
                                            {DependencyEdgeId(1), DependencyEdgeId(2)}),
                                    f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    f.g.recompute_all_readiness();
    CHECK_EQ(f.ready(3), ReadinessState::DEGRADED);
  }

  return summary("depset");
}
