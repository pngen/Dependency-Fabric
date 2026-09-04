// dependency_fabric::df_cli — graph inspection CLI.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Inspect a persisted graph: node/edge/set counts, echo the structure, per-node
// readiness + blockers, and the deterministic graph digest.

#include <cstdio>
#include <cstring>
#include <string>
#include "dependency_fabric/df.hpp"

using namespace dependency_fabric;

int main(int argc, char** argv) {
  std::string path;
  std::string mode = "summary";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--load" && i + 1 < argc) path = argv[++i];
    else if (a == "--mode" && i + 1 < argc) mode = argv[++i];
    else if (a == "--help" || a == "-h") {
      std::printf("usage: df_cli --load <graph.bin> [--mode summary|full]\n");
      return 0;
    }
  }
  if (path.empty()) { std::fprintf(stderr, "error: --load <path> is required\n"); return 1; }

  Graph g;
  std::string err;
  if (Persistence::load_file(path, &g, &err) != Outcome::ACCEPTED) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }

  std::printf("Dependency Fabric graph %s\n", path.c_str());
  std::printf("  nodes=%zu edges=%zu sets=%zu sources=%zu digest=%llu (0x%llX)\n",
              g.node_count(), g.edge_count(), g.set_count(), g.source_count(),
              static_cast<unsigned long long>(g.digest()),
              static_cast<unsigned long long>(g.digest()));
  std::printf("  acyclic=%s\n", g.acyclic() ? "yes" : "NO");
  if (mode != "full") return 0;

  for (auto nid : g.all_nodes()) {
    auto n = g.node(nid);
    if (!n) continue;
    ReadinessState st = g.readiness(nid);
    DependencyNodeGeneration gen = g.current_authority(nid);
    std::printf("node %llu kind=%s state=%s gen=%llu desc=%s\n",
                static_cast<unsigned long long>(nid.value()),
                std::string(name_of(n->kind)).c_str(),
                std::string(name_of(st)).c_str(),
                static_cast<unsigned long long>(gen.value()),
                n->descriptor.c_str());
    auto bl = g.blockers(nid);
    for (const auto& b : bl) {
      std::printf("    blocked by edge %llu -> node %llu: %s\n",
                  static_cast<unsigned long long>(b.edge_id.value()),
                  static_cast<unsigned long long>(b.producer.value()),
                  b.reason.c_str());
    }
  }
  for (auto eid : g.all_edges()) {
    auto e = g.edge(eid);
    if (!e) continue;
    std::printf("edge %llu kind=%s producer=%llu consumer=%llu active=%s\n",
                static_cast<unsigned long long>(eid.value()),
                std::string(name_of(e->kind)).c_str(),
                static_cast<unsigned long long>(e->producer_id.value()),
                static_cast<unsigned long long>(e->consumer_id.value()),
                e->active ? "yes" : "no");
  }
  return 0;
}
