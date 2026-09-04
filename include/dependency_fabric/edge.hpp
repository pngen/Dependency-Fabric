// dependency_fabric::edge — explicit dependency edges.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "identity.hpp"
#include "enums.hpp"
#include "predicates.hpp"

namespace dependency_fabric {

struct Edge {
  DependencyEdgeId id;
  DependencyEdgeGeneration generation;

  EdgeKind kind = EdgeKind::REQUIRES;

  // producer_id is the prerequisite/source; consumer_id is the dependent/target.
  DependencyNodeId producer_id;
  DependencyNodeId consumer_id;

  // The generation binding at which this edge was established. These pin a
  // producer/consumer generation to the edge (REQUIRES_GENERATION).
  ProducerGeneration bound_producer_generation;
  ConsumerGeneration bound_consumer_generation;

  DependencyPredicate predicate;

  // A retired/inactive edge is preserved for append-only history but no longer
  // participates in readiness evaluation.
  bool active = true;

  // A hard dependency blocks its consumer when unsatisfied. OPTIONAL edges are
  // never hard; REQUIRES_* edges are hard unless explicitly marked otherwise.
  bool required() const noexcept {
    return kind != EdgeKind::OPTIONAL && predicate.is_required;
  }

  // Every edge kind in the current taxonomy encodes a reachability relation.
  bool reachability_relation() const noexcept {
    switch (kind) {
      case EdgeKind::PRODUCES:
      case EdgeKind::DERIVES_FROM:
      case EdgeKind::INVALIDATES_WITH:
      case EdgeKind::REQUIRES:
      case EdgeKind::REQUIRES_READY:
      case EdgeKind::REQUIRES_FRESH:
      case EdgeKind::REQUIRES_GENERATION:
      case EdgeKind::REQUIRES_RESOURCE:
      case EdgeKind::REQUIRES_CAPABILITY:
      case EdgeKind::COMPATIBLE_WITH:
      case EdgeKind::OPTIONAL:
        return true;
    }
    return true;
  }
};

}  // namespace dependency_fabric
