// dependency_fabric::test — adversarial hardening: deep graphs, wide fan-out,
// integer overflow, stale replay, malformed frames. We try to break the graph.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "test_util.hpp"
#include "dependency_fabric/protocol.hpp"

#include <limits>

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
  // --- deep chain: no stack exhaustion, descendants blocked ------------------
  {
    tf::Fixture f;
    const std::uint64_t D = 2000;
    for (std::uint64_t i = 1; i <= D; ++i) {
      CHECK_EQ(f.declare(i, NodeKind::MODEL, ""), Outcome::ACCEPTED);
      CHECK_EQ(f.pub(i, 1), Outcome::ACCEPTED);
    }
    for (std::uint64_t i = 1; i < D; ++i) {
      CHECK_EQ(f.g.add_edge(ready_edge(100000 + i, i, i + 1), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    }
    f.g.recompute_all_readiness();
    CHECK_EQ(f.ready(D), ReadinessState::READY);
    InvalidationPolicy pol; pol.mode = InvalidationMode::RECURSIVE;
    CHECK_EQ(f.g.invalidate_node(DependencyNodeId(1000), InvalidationReason::UPSTREAM_GONE, pol, f.src, f.boot, f.epoch),
             Outcome::ACCEPTED);
    CHECK_EQ(f.ready(1000), ReadinessState::INVALIDATED);
    // A downstream leaf after the invalidated node must be BLOCKED.
    CHECK_EQ(f.ready(1500), ReadinessState::BLOCKED);
    CHECK(f.g.acyclic());
  }

  // --- wide fan-out: one model with many dependents --------------------------
  {
    tf::Fixture f;
    f.declare(1, NodeKind::MODEL, "root");
    f.pub(1, 1);
    const std::uint64_t W = 3000;
    for (std::uint64_t i = 2; i <= W + 1; ++i) {
      CHECK_EQ(f.declare(i, NodeKind::ARTIFACT, ""), Outcome::ACCEPTED);
      CHECK_EQ(f.pub(i, 1), Outcome::ACCEPTED);
      CHECK_EQ(f.g.add_edge(ready_edge(200000 + i, 1, i), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    }
    f.g.recompute_all_readiness();
    CHECK_EQ(f.ready(2), ReadinessState::READY);
    CHECK_EQ(f.ready(W + 1), ReadinessState::READY);
    InvalidationPolicy pol; pol.mode = InvalidationMode::RECURSIVE;
    CHECK_EQ(f.g.invalidate_node(DependencyNodeId(1), InvalidationReason::UPSTREAM_GONE, pol, f.src, f.boot, f.epoch),
             Outcome::ACCEPTED);
    CHECK_EQ(f.ready(2), ReadinessState::BLOCKED);
    CHECK_EQ(f.ready(W + 1), ReadinessState::BLOCKED);
  }

  // --- integer overflow on generation advance ---------------------------------
  {
    tf::Fixture f;
    f.declare(1, NodeKind::MODEL, "m");
    CHECK_EQ(f.g.publish_generation(DependencyNodeId(1),
             DependencyNodeGeneration(std::numeric_limits<std::uint64_t>::max()), f.src, f.boot, f.epoch),
             Outcome::ACCEPTED);
    // Advancing past max would wrap to 0; must be rejected, not accepted.
    // A zero (null) generation is invalid; a *valid* lower generation is stale.
    CHECK_EQ(f.g.publish_generation(DependencyNodeId(1), DependencyNodeGeneration(0), f.src, f.boot, f.epoch),
             Outcome::REJECT_INVALID_TRANSITION);
    CHECK_EQ(f.g.publish_generation(DependencyNodeId(1), DependencyNodeGeneration(5), f.src, f.boot, f.epoch),
             Outcome::REJECT_STALE_NODE_GENERATION);
    CHECK_EQ(f.g.current_authority(DependencyNodeId(1)).value(), std::numeric_limits<std::uint64_t>::max());
  }

  // --- stale boot replay ------------------------------------------------------
  {
    tf::Fixture f;
    // Register source 1 with boot 1, then re-register with boot 2 (fresh).
    CHECK_EQ(f.g.register_source(SourceId(9), SourceBootId(1), CoordinatorEpoch(1)), Outcome::ACCEPTED);
    CHECK_EQ(f.g.register_source(SourceId(9), SourceBootId(2), CoordinatorEpoch(1)), Outcome::ACCEPTED);
    CHECK_EQ(f.declare(1, NodeKind::MODEL, "m"), Outcome::ACCEPTED);
    // Old boot is now stale.
    CHECK_EQ(f.g.declare_node(DependencyNodeId(2), NodeKind::ARTIFACT, "x", SourceId(9), SourceBootId(1), CoordinatorEpoch(1)),
             Outcome::REJECT_STALE_BOOT);
    // Fresh boot works.
    CHECK_EQ(f.g.declare_node(DependencyNodeId(2), NodeKind::ARTIFACT, "x", SourceId(9), SourceBootId(2), CoordinatorEpoch(1)),
             Outcome::ACCEPTED);
  }

  // --- removing nonexistent objects -------------------------------------------
  {
    tf::Fixture f;
    f.declare(1, NodeKind::MODEL, "m"); f.pub(1, 1);
    CHECK_EQ(f.g.remove_edge(DependencyEdgeId(999), f.src, f.boot, f.epoch), Outcome::REJECT_UNKNOWN_DEPENDENCY);
    CHECK_EQ(f.g.remove_dependency_set(DependencySetId(999), f.src, f.boot, f.epoch), Outcome::REJECT_UNKNOWN_DEPENDENCY);
    CHECK_EQ(f.g.readiness(DependencyNodeId(999)), ReadinessState::UNKNOWN);
    // Retire then recompute_stays RETIRED (escape state preserved).
    CHECK_EQ(f.g.retire_node(DependencyNodeId(1), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
    CHECK_EQ(f.g.recompute_all_readiness(), Outcome::ACCEPTED);
    CHECK_EQ(f.ready(1), ReadinessState::RETIRED);
  }

  // --- protocol frame hardening -----------------------------------------------
  {
    using namespace dependency_fabric::proto;
    Frame f(FrameType::REGISTER_REQ, std::vector<std::uint8_t>(64, 0xAB));
    auto enc = encode_frame(f);
    CHECK(enc.has_value());
    auto dec = decode_frame(enc->data(), enc->size());
    CHECK(dec.has_value());
    CHECK(dec->type == FrameType::REGISTER_REQ);
    CHECK(dec->body.size() == 64u);

    // Truncated frame rejected.
    CHECK(!decode_frame(enc->data(), enc->size() - 1).has_value());
    // Body length too large rejected by decode.
    auto too_big = Frame(FrameType::REGISTER_REQ, std::vector<std::uint8_t>(kMaxBodyBytes + 1, 0));
    CHECK(!encode_frame(too_big).has_value());
    // Checksum mismatch -> rejected.
    auto bad = *enc;
    bad[bad.size() - 1] ^= 0x55;
    CHECK(!decode_frame(bad.data(), bad.size()).has_value());
    // Bad magic.
    auto badmagic = *enc;
    badmagic[0] = 0;
    CHECK(!decode_frame(badmagic.data(), badmagic.size()).has_value());
  }

  return summary("adversarial");
}