// dependency_fabric::test utility — shared fixtures.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "dependency_fabric/df.hpp"
#include "test_framework.hpp"

namespace tf {

using namespace dependency_fabric;

// A graph fixture bound to a single registered source at the genesis epoch.
struct Fixture {
  Graph g;
  const SourceId src{1};
  const SourceBootId boot{1};
  const CoordinatorEpoch epoch{1};

  Fixture() { (void)g.register_source(src, boot, epoch); }
  Fixture(const Fixture&) = delete;

  Outcome declare(std::uint64_t id, NodeKind kind, const char* desc) {
    return g.declare_node(DependencyNodeId(id), kind, desc, src, boot, epoch);
  }
  Outcome declare_static(std::uint64_t id, NodeKind kind, const char* desc) {
    return g.declare_node(DependencyNodeId(id), kind, desc, src, boot, epoch, true);
  }
  Outcome pub(std::uint64_t id, std::uint64_t gen) {
    return g.publish_generation(DependencyNodeId(id), DependencyNodeGeneration(gen), src, boot, epoch);
  }
  Outcome add(std::uint64_t id, std::uint64_t prod, std::uint64_t cons, std::uint64_t pinned,
              EdgeKind kind = EdgeKind::REQUIRES_GENERATION, bool required = true) {
    Edge e = pin_edge(id, prod, cons, pinned, kind, required);
    return g.add_edge(e, src, boot, epoch);
  }
  ReadinessState ready(std::uint64_t id) const { return g.readiness(DependencyNodeId(id)); }

  static Edge pin_edge(std::uint64_t id, std::uint64_t prod, std::uint64_t cons, std::uint64_t pinned,
                       EdgeKind kind = EdgeKind::REQUIRES_GENERATION, bool required = true) {
    Edge e;
    e.id = DependencyEdgeId(id);
    e.generation = DependencyEdgeGeneration(1);
    e.kind = kind;
    e.producer_id = DependencyNodeId(prod);
    e.consumer_id = DependencyNodeId(cons);
    e.predicate.generation_kind = GenerationPredicateKind::EXACT;
    e.predicate.exact_generation = DependencyNodeGeneration(pinned);
    e.predicate.required_readiness = ReadinessState::READY;
    e.predicate.is_required = required;
    return e;
  }
};

}  // namespace tf