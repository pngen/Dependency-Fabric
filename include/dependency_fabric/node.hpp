// dependency_fabric::node — strongly typed dependency nodes.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A node holds:
//  * a typed identity (DependencyNodeId),
//  * a typed kind (NodeKind),
//  * an append-only generation history plus a current authoritative generation,
//  * a current readiness state,
//  * per-authority-domain generation axes (artifact/state/resource/…),
//  * evidence facts consulted by predicate evaluation,
//  * provenance of the source that produced the current state.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "identity.hpp"
#include "enums.hpp"

namespace dependency_fabric {

struct Node {
  DependencyNodeId id;
  NodeKind kind = NodeKind::UNKNOWN;

  // Append-only generation history. The last element is the newest generation.
  std::vector<DependencyNodeGeneration> generation_history;

  // The current authoritative generation.
  DependencyNodeGeneration current_generation;

  // Per-authority-domain generation axes. These record which artifact/state/
  // resource generation this node was validated against. They live in their own
  // typed domains and are never folded into the node generation.
  ArtifactGeneration artifact_generation;
  StateGeneration state_generation;
  ResourceGeneration resource_generation;

  // Monotonic axes that advance on events.
  ReadinessGeneration readiness_generation;
  InvalidationGeneration invalidation_generation;
  RecoveryGeneration recovery_generation;
  PolicyGeneration policy_generation;

  ReadinessState state = ReadinessState::UNKNOWN;

  // Static durable facts remain durable across coordinator restart.
  bool static_durable_fact = false;
  // Dynamic runtime evidence is *not* assumed durable across restart.
  bool dynamic_evidence = false;
  DynamicEvidence recovered_evidence = DynamicEvidence::UNKNOWN;
  // Whether this node was READY before a coordinator restart.
  bool was_ready_before_restart = false;

  // Provenance: which source (and which boot incarnation) produced this state.
  SourceId origin_source;
  SourceBootId origin_boot;

  // Superseded generations (preserved for history).
  std::vector<DependencyNodeGeneration> superseded_generations;

  // Evidence facts consulted by predicate evaluation when no external adapter
  // is attached. An attached adapter may override these.
  std::vector<std::string> claimed_capabilities;
  bool resource_available = false;
  bool integrity_valid = false;
  bool freshness_valid = false;
  std::uint64_t provenance_confidence = 0;
  std::string completion_state;

  // The last invalidation reason recorded for this node (for explanations).
  InvalidationReason last_invalidation_reason = InvalidationReason::EXPLICIT;
  bool ever_invalidated = false;

  std::string descriptor;

  explicit Node(DependencyNodeId node_id = DependencyNodeId(),
                NodeKind node_kind = NodeKind::UNKNOWN,
                std::string desc = std::string())
      : id(node_id), kind(node_kind), descriptor(std::move(desc)) {}

  [[nodiscard]] DependencyNodeGeneration highest_generation() const noexcept {
    if (!generation_history.empty()) return generation_history.back();
    return current_generation;
  }

  [[nodiscard]] bool has_generation(DependencyNodeGeneration g) const noexcept {
    for (const auto& h : generation_history) {
      if (h.value() == g.value()) return true;
    }
    return false;
  }
};

// A source registry entry: a source identity is bound to the current boot
// incarnation and the epoch at which it was registered.
struct SourceRegistration {
  SourceId source;
  SourceBootId boot;
  CoordinatorEpoch epoch;
  bool active = true;
};

}  // namespace dependency_fabric
