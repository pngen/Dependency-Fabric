// dependency_fabric::queries — inspectable query and explanation results.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "identity.hpp"
#include "enums.hpp"
#include "edge.hpp"
#include "node.hpp"

namespace dependency_fabric {

// A single unsatisfied (or degraded) dependency that is contributing to a
// consumer's non-READY state.
struct BlockingDependency {
  DependencyEdgeId edge_id;
  DependencyEdgeGeneration edge_generation;
  DependencyNodeId producer;
  NodeKind producer_kind = NodeKind::UNKNOWN;
  EdgeKind kind = EdgeKind::REQUIRES;
  bool optional = false;
  bool is_set_member = false;
  std::string set_label;

  // The constraint that failed.
  GenerationPredicateKind constraint_kind = GenerationPredicateKind::NONE;
  DependencyNodeGeneration required_generation;
  DependencyNodeGeneration actual_generation;

  // Human-readable reason, e.g. "producer generation X does not satisfy EXACT Y".
  std::string reason;
};

// A full, inspectable explanation of a node's current state.
struct Explanation {
  DependencyNodeId node;
  NodeKind kind = NodeKind::UNKNOWN;
  Outcome outcome = Outcome::UNKNOWN;
  ReadinessState state = ReadinessState::UNKNOWN;
  DependencyNodeGeneration current_generation;
  SourceId origin_source;
  SourceBootId origin_boot;

  std::string summary;
  std::vector<BlockingDependency> blockers;

  // Which upstream invalidation(s) most recently affected this node.
  std::vector<std::string> upstream_invalidation_notes;

  // What changed since this node was last READY (generation deltas).
  std::vector<std::string> changes_since_ready;

  // Recovery/execution obligations.
  std::vector<std::string> required_actions;
};

// A structured recovery obligation returned to an owning runtime. Dependency
// Fabric never performs the recovery action itself; it only determines what
// obligation now exists.
struct RecoveryIntent {
  DependencyNodeId node;
  NodeKind kind = NodeKind::UNKNOWN;
  RecoveryAction action = RecoveryAction::REVALIDATION;
  InvalidationReason reason = InvalidationReason::EXPLICIT;
  std::string owner_runtime;  // e.g. "CheckpointStore", "ArtifactFabric", ...
  std::string detail;
};

// An ordered recovery plan. Ordering respects dependency topology: a node's
// recovery obligations precede those of its dependents.
struct RecoveryPlan {
  bool any() const { return !intents.empty(); }
  std::vector<RecoveryIntent> intents;
};

// The affected closure of an invalidation or generation change.
struct AffectedClosure {
  std::vector<DependencyNodeId> nodes;
  std::vector<DependencyEdgeId> edges;
  // The recomputation frontier: nodes whose dependents must be recomputed,
  // i.e. the invalidated subgraph's leaves (nodes with no affected dependents).
  std::vector<DependencyNodeId> frontier;
};

// A deterministic traversal result (used for ancestors/descendants sets).
using NodeList = std::vector<DependencyNodeId>;

}  // namespace dependency_fabric
