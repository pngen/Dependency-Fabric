// dependency_fabric::test — property-based graph testing over randomized DAGs
// and event sequences.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "test_util.hpp"

#include <algorithm>
#include <random>
#include <set>

using namespace df_test;
using namespace dependency_fabric;

namespace {
struct Lcg {
  std::uint64_t s;
  explicit Lcg(std::uint64_t seed) : s(seed) {}
  std::uint64_t next() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return s;
  }
  std::uint64_t range(std::uint64_t n) { return n ? next() % n : 0; }
};

bool has_invalid_ancestor(const Graph& g, DependencyNodeId node) {
  auto anc = g.prerequisites(node, true);
  for (auto a : anc) {
    auto nd = g.node(a);
    if (nd && (nd->state == ReadinessState::INVALIDATED || nd->state == ReadinessState::BLOCKED ||
               nd->state == ReadinessState::MISSING || nd->state == ReadinessState::STALE)) {
      return true;
    }
  }
  return false;
}

bool history_monotonic(const Graph& g) {
  for (auto nid : g.all_nodes()) {
    auto n = g.node(nid);
    if (!n || n->generation_history.size() < 2) continue;
    for (std::size_t i = 1; i < n->generation_history.size(); ++i) {
      if (n->generation_history[i].value() <= n->generation_history[i - 1].value()) return false;
    }
    if (n->current_generation.value() != n->generation_history.back().value()) return false;
  }
  return true;
}
}  // namespace

int main() {
  const std::uint64_t rounds = 40;
  for (std::uint64_t round = 0; round < rounds; ++round) {
    Lcg rng(0x5eed + round * 7919);
    const std::size_t n = 8 + rng.range(30);
    tf::Fixture f;

    std::vector<std::uint64_t> node_ids;
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint64_t id = i + 1;
      node_ids.push_back(id);
      CHECK_EQ(f.declare(id, NodeKind::MODEL, ""), Outcome::ACCEPTED);
    }

    // Publish initial generations and add random acyclic edges (producer<consumer).
    const std::uint64_t edge_density = 1 + rng.range(4);
    for (auto id : node_ids) {
      if (rng.range(2) == 0) CHECK_EQ(f.pub(id, 1), Outcome::ACCEPTED);
    }
    for (std::size_t i = 0; i < n; ++i) {
      for (std::size_t j = i + 1; j < n; ++j) {
        if (rng.range(edge_density * 4) == 0) {
          // producer = node_ids[i], consumer = node_ids[j] (producer<consumer index)
          CHECK(f.g.add_edge(tf::Fixture::pin_edge(1000 + i * 100 + j, node_ids[i], node_ids[j], 1),
                             f.src, f.boot, f.epoch) == Outcome::ACCEPTED ||
                f.g.add_edge(tf::Fixture::pin_edge(1000 + i * 100 + j, node_ids[i], node_ids[j], 1),
                             f.src, f.boot, f.epoch) == Outcome::REJECT_DUPLICATE_ID);
        }
      }
    }
    f.g.recompute_all_readiness();
    CHECK(f.g.acyclic());
    CHECK(history_monotonic(f.g));

    // Random event sequence.
    const std::uint64_t events = 30;
    for (std::uint64_t e = 0; e < events; ++e) {
      const std::uint64_t kind = rng.range(5);
      const std::uint64_t id = node_ids[rng.range(n)];
      switch (kind) {
        case 0: {  // publish next generation (monotonic)
          auto cur = f.g.current_authority(DependencyNodeId(id));
          const std::uint64_t ngen = cur.value() + 1;
          CHECK_EQ(f.pub(id, ngen), Outcome::ACCEPTED);
          break;
        }
        case 1: {  // recursive invalidation
          InvalidationPolicy pol;
          pol.mode = (rng.range(2) ? InvalidationMode::RECURSIVE : InvalidationMode::BOUNDED);
          pol.max_depth = 3;
          CHECK_EQ(f.g.invalidate_node(DependencyNodeId(id), InvalidationReason::EXPLICIT, pol, f.src, f.boot, f.epoch),
                   Outcome::ACCEPTED);
          break;
        }
        case 2: {  // mark stale
          CHECK_EQ(f.g.mark_stale(DependencyNodeId(id), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
          break;
        }
        case 3: {  // query readiness (read-only, must not throw)
          (void)f.g.readiness(DependencyNodeId(id));
          (void)f.g.blockers(DependencyNodeId(id));
          break;
        }
        case 4: {  // recover
          CHECK_EQ(f.g.mark_recovered(DependencyNodeId(id), f.src, f.boot, f.epoch), Outcome::ACCEPTED);
          break;
        }
      }

      // Invariants after each event-block.
      CHECK(f.g.acyclic());
      CHECK(history_monotonic(f.g));
      // Stale sources cannot mutate: re-using a stale boot is rejected.
      CHECK_EQ(f.g.publish_generation(DependencyNodeId(id), DependencyNodeGeneration(9999),
                                      f.src, SourceBootId(12345), f.epoch),
               Outcome::REJECT_STALE_BOOT);
    }

    // Final: descendants cannot be READY while a required ancestor is invalid.
    for (auto nid : node_ids) {
      if (f.g.readiness(DependencyNodeId(nid)) == ReadinessState::READY) {
        CHECK(!has_invalid_ancestor(f.g, DependencyNodeId(nid)));
      }
    }

    // Persistence round-trip preserves durable topology.
    auto bytes = f.g.serialize();
    CHECK(bytes.has_value());
    Graph g2;
    CHECK_EQ(Persistence::load(*bytes, &g2, nullptr), Outcome::ACCEPTED);
    CHECK_EQ(g2.node_count(), f.g.node_count());
    CHECK_EQ(g2.edge_count(), f.g.edge_count());
    CHECK(g2.acyclic());
  }

  return summary("property");
}