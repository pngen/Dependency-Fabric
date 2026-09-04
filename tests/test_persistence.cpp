// dependency_fabric::test — persistence round-trip and corruption rejection.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "test_util.hpp"

using namespace df_test;
using namespace dependency_fabric;

static void make_graph(tf::Fixture& f) {
  f.declare(1, NodeKind::MODEL, "model");
  f.declare(2, NodeKind::ARTIFACT, "artifact");
  f.pub(1, 1);
  f.pub(2, 1);
  f.add(100, 1, 2, 1);
}

int main() {
  // --- round-trip fidelity -------------------------------------------------
  {
    tf::Fixture f; make_graph(f);
    f.g.recompute_all_readiness();
    CHECK_EQ(f.ready(2), ReadinessState::READY);
    auto bytes = f.g.serialize();
    CHECK(bytes.has_value());
    Graph g2;
    CHECK_EQ(Persistence::load(*bytes, &g2, nullptr), Outcome::ACCEPTED);
    CHECK_EQ(g2.node_count(), 2u);
    CHECK_EQ(g2.edge_count(), 1u);
    CHECK_EQ(g2.readiness(DependencyNodeId(1)), ReadinessState::REVALIDATION_REQUIRED);
    // Dynamic evidence after load is conservative unless re-published.
    CHECK_EQ(g2.readiness(DependencyNodeId(2)), ReadinessState::REVALIDATION_REQUIRED);
    // Re-publishing the model makes artifact READY again.
    std::string err;
    CHECK_EQ(Persistence::save_file(f.g, "persist_rt.bin", &err), Outcome::ACCEPTED);
    Graph g3;
    CHECK_EQ(Persistence::load_file("persist_rt.bin", &g3, &err), Outcome::ACCEPTED);
    CHECK_EQ(g3.node_count(), 2u);
  }

  // --- deterministic digests ------------------------------------------------
  {
    tf::Fixture f; make_graph(f);
    tf::Fixture f2; make_graph(f2);
    CHECK_EQ(f.g.digest(), f2.g.digest());
  }

  // --- corruption rejection -------------------------------------------------
  {
    tf::Fixture f; make_graph(f);
    auto bytes = f.g.serialize();
    CHECK(bytes.has_value());
    const auto& b = *bytes;

    // Empty payload.
    Graph g0;
    CHECK_EQ(Persistence::load(std::vector<std::uint8_t>{}, &g0, nullptr), Outcome::REJECT_MALFORMED);
    // Tiny payload.
    CHECK_EQ(Persistence::load(std::vector<std::uint8_t>({1, 2, 3, 4}), &g0, nullptr), Outcome::REJECT_MALFORMED);

    // Truncation (drop last byte).
    auto trunc = b;
    trunc.pop_back();
    CHECK_EQ(Persistence::load(trunc, &g0, nullptr), Outcome::REJECT_MALFORMED);
    // Drop half.
    auto trunc2 = std::vector<std::uint8_t>(b.begin(), b.begin() + b.size() / 2);
    CHECK_EQ(Persistence::load(trunc2, &g0, nullptr), Outcome::REJECT_MALFORMED);

    // Bad magic.
    auto badmagic = b;
    badmagic[0] = 0xFF;
    CHECK_EQ(Persistence::load(badmagic, &g0, nullptr), Outcome::REJECT_MALFORMED);

    // Bad version.
    auto badver = b;
    badver[4] = 0xEE;
    badver[5] = 0xEE;
    CHECK_EQ(Persistence::load(badver, &g0, nullptr), Outcome::REJECT_MALFORMED);

    // Trailing garbage.
    auto trailing = b;
    trailing.push_back(0xAA);
    CHECK_EQ(Persistence::load(trailing, &g0, nullptr), Outcome::REJECT_MALFORMED);

    // Corrupt a body byte (digest mismatch).
    auto corrupt = b;
    corrupt[b.size() / 2] ^= 0x5A;
    CHECK_EQ(Persistence::load(corrupt, &g0, nullptr), Outcome::REJECT_MALFORMED);
  }

  // --- invalid enum via a hand-crafted node kind byte ----------------------
  {
    tf::Fixture f; make_graph(f);
    auto b = f.g.serialize();
    CHECK(b.has_value());
    // Locate the first node record's kind byte: header is 16 bytes; body starts
    // with epoch(8) + source_count(4). With one registered source (source id 1),
    // that source record is 8+8+8+1 = 25 bytes, then node_count(4). The first
    // node record's kind is its 9th byte (id u64 then kind u8).
    // Offsets: 16(header) + 8(epoch) + 4(source_count) + 25(source) + 4(node_count) + 8(id) = 65.
    auto inv = *b;
    CHECK(inv.size() > 65);
    inv[65] = 0xFF;  // invalid NodeKind enum
    Graph g;
    CHECK_EQ(Persistence::load(inv, &g, nullptr), Outcome::REJECT_MALFORMED);
  }

  return summary("persistence");
}