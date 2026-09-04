// dependency_fabric::enums — typed node kinds, edge kinds, readiness states,
// generation predicates, set operators, outcomes, recovery actions, and
// invalidation reasons.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string_view>
#include <array>
#include <optional>

namespace dependency_fabric {

// ---------------------------------------------------------------------------
// Node kinds.
// ---------------------------------------------------------------------------
enum class NodeKind : std::uint8_t {
  DATA,            // raw data payload
  TENSOR,          // tensor object
  KV_STATE,        // key/value cache state
  CHECKPOINT,      // checkpoint artifact
  MODEL,           // model weights / architecture
  ADAPTER,         // fine-tune / adaptation
  KERNEL,          // compiled kernel
  EXECUTION_GRAPH,// execution graph structure
  ARTIFACT,        // generic artifact
  SERVICE,         // runtime service
  RESOURCE,        // physical or allocatable resource
  TOPOLOGY,        // hardware / deployment topology
  CAPABILITY,      // capability requirement / claim
  EXECUTION_RESULT,// upstream execution result
  WORKLOAD_PHASE,  // workload lifecycle phase
  POLICY,          // policy generation
  UNKNOWN,         // unclassified
};

enum class EdgeKind : std::uint8_t {
  REQUIRES,            // consumer requires producer (hard)
  OPTIONAL,            // consumer optionally uses producer
  PRODUCES,            // producer node generates the consumer node
  DERIVES_FROM,        // consumer derives from producer
  INVALIDATES_WITH,    // producer invalidation invalidates consumer
  COMPATIBLE_WITH,     // compatibility relationship
  REQUIRES_RESOURCE,   // consumer requires producer resource
  REQUIRES_CAPABILITY, // consumer requires producer capability
  REQUIRES_READY,      // consumer requires producer readiness
  REQUIRES_FRESH,      // consumer requires producer freshness
  REQUIRES_GENERATION, // consumer requires a producer generation constraint
};

enum class ReadinessState : std::uint8_t {
  UNKNOWN,        // identity known, no evidence
  DECLARED,       // declared but not yet published
  DISCOVERED,     // discovered by inspection
  PRESENT,        // evidence present, not yet validated
  VALIDATING,     // integrity/validation in progress
  READY,          // all required dependency predicates satisfied
  DEGRADED,       // functional but with degraded optional/freshness semantics
  STALE,          // previously valid, now behind authoritative generation
  INVALIDATED,    // explicitly invalidated
  REVALIDATION_REQUIRED, // dynamic evidence must be re-corroborated
  MISSING,        // expected but absent
  FAILED,         // validation or acquisition failed
  RECOVERING,     // recovery in flight
  BLOCKED,        // required dependency unsatisfied
  SUPERSEDED,     // a newer generation is authoritative
  RETIRED,        // intentionally retired
};

// Special runtime evidence states produced after recovery/restart. These are
// *not* durable "READY" claims; dynamic evidence must be re-corroborated.
enum class DynamicEvidence : std::uint8_t {
  REVALIDATION_REQUIRED,
  STALE,
  UNKNOWN,
  FRESH,
};

enum class GenerationPredicateKind : std::uint8_t {
  EXACT,          // producer generation must equal a specific value
  MINIMUM,        // producer generation must be >= a specific value
  COMPATIBLE_SET, // producer generation must be one of a set of values
  NONE,           // no generation constraint
};

enum class SetOperator : std::uint8_t {
  ALL_OF,        // every member must be satisfied
  ANY_OF,        // at least one member must be satisfied
  AT_LEAST_N,    // at least N members must be satisfied
  OPTIONAL_GROUP,// members are optional; group never hard-blocks
};

enum class Outcome : std::uint8_t {
  READY,
  BLOCKED,
  DEGRADED,
  REVALIDATION_REQUIRED,
  RECOMPUTE_REQUIRED,
  RECOVERY_REQUIRED,
  REJECT_STALE_EPOCH,
  REJECT_STALE_BOOT,
  REJECT_STALE_NODE_GENERATION,
  REJECT_STALE_EDGE_GENERATION,
  REJECT_CYCLE,
  REJECT_DUPLICATE_ID,
  REJECT_INVALID_TRANSITION,
  REJECT_UNKNOWN_DEPENDENCY,
  REJECT_MALFORMED,
  UNKNOWN,
  ACCEPTED,
};

enum class RecoveryAction : std::uint8_t {
  REVALIDATION,           // re-validate evidence against current authority
  RECOMPUTATION,          // recompute the node content
  RELOAD,                 // reload a persisted artifact
  RESTART,                // restart a workload episode/service
  REBIND,                 // rebind to a fresh dependency generation
  RESOURCE_REACQUISITION, // reacquire a resource
  CHECKPOINT_RESTORE,     // restore checkpoint bytes
  MANUAL_INTERVENTION,    // human/operator action required
};

enum class InvalidationReason : std::uint8_t {
  EXPLICIT,                        // explicit user invalidation
  UPSTREAM_GONE,                   // upstream node disappeared
  UPSTREAM_GENERATION_CHANGED,     // authoritative generation advanced
  COMPATIBILITY_CHANGED,           // compatibility decision changed
  RESOURCE_UNAVAILABLE,            // resource became unavailable
  FRESHNESS_EXPIRED,               // freshness window elapsed
  INTEGRITY_FAILED,                // integrity/corruption check failed
  AUTHORITY_ADVANCED,              // coordinator/source authority advanced
  SUPERSEDED,                      // superseded generation
};

// How an invalidation propagates.
enum class InvalidationMode : std::uint8_t {
  DIRECT,     // only this node
  RECURSIVE,  // all descendants following hard edges
  BOUNDED,    // descendants up to max_depth
  GENERATION, // invalidate a specific generation (and its descendants)
};

// Policy that governs propagation of an invalidation event.
struct InvalidationPolicy {
  InvalidationMode mode = InvalidationMode::RECURSIVE;
  std::uint32_t max_depth = 0;                  // 0 = unlimited (for BOUNDED)
  bool propagate_through_optional = false;      // reach through OPTIONAL edges
  bool degrade_optional_dependents = false;     // mark optional dependents DEGRADED not INVALIDATED
  bool cascade_blocked = true;                  // block descendants of invalidated nodes
};

enum class StaleDisposition : std::uint8_t {
  REQUIRE_REVALIDATION, // must re-validate dynamic evidence
  REQUIRE_RESOURCE,     // must reacquire resource
  REQUIRE_NONE,         // static durable facts remain valid
};

// ---------------------------------------------------------------------------
// Name tables and parsers.
// ---------------------------------------------------------------------------
constexpr std::string_view name_of(NodeKind k) {
  switch (k) {
    case NodeKind::DATA: return "DATA";
    case NodeKind::TENSOR: return "TENSOR";
    case NodeKind::KV_STATE: return "KV_STATE";
    case NodeKind::CHECKPOINT: return "CHECKPOINT";
    case NodeKind::MODEL: return "MODEL";
    case NodeKind::ADAPTER: return "ADAPTER";
    case NodeKind::KERNEL: return "KERNEL";
    case NodeKind::EXECUTION_GRAPH: return "EXECUTION_GRAPH";
    case NodeKind::ARTIFACT: return "ARTIFACT";
    case NodeKind::SERVICE: return "SERVICE";
    case NodeKind::RESOURCE: return "RESOURCE";
    case NodeKind::TOPOLOGY: return "TOPOLOGY";
    case NodeKind::CAPABILITY: return "CAPABILITY";
    case NodeKind::EXECUTION_RESULT: return "EXECUTION_RESULT";
    case NodeKind::WORKLOAD_PHASE: return "WORKLOAD_PHASE";
    case NodeKind::POLICY: return "POLICY";
    case NodeKind::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}

constexpr std::string_view name_of(EdgeKind k) {
  switch (k) {
    case EdgeKind::REQUIRES: return "REQUIRES";
    case EdgeKind::OPTIONAL: return "OPTIONAL";
    case EdgeKind::PRODUCES: return "PRODUCES";
    case EdgeKind::DERIVES_FROM: return "DERIVES_FROM";
    case EdgeKind::INVALIDATES_WITH: return "INVALIDATES_WITH";
    case EdgeKind::COMPATIBLE_WITH: return "COMPATIBLE_WITH";
    case EdgeKind::REQUIRES_RESOURCE: return "REQUIRES_RESOURCE";
    case EdgeKind::REQUIRES_CAPABILITY: return "REQUIRES_CAPABILITY";
    case EdgeKind::REQUIRES_READY: return "REQUIRES_READY";
    case EdgeKind::REQUIRES_FRESH: return "REQUIRES_FRESH";
    case EdgeKind::REQUIRES_GENERATION: return "REQUIRES_GENERATION";
  }
  return "REQUIRES";
}

constexpr std::string_view name_of(ReadinessState s) {
  switch (s) {
    case ReadinessState::UNKNOWN: return "UNKNOWN";
    case ReadinessState::DECLARED: return "DECLARED";
    case ReadinessState::DISCOVERED: return "DISCOVERED";
    case ReadinessState::PRESENT: return "PRESENT";
    case ReadinessState::VALIDATING: return "VALIDATING";
    case ReadinessState::READY: return "READY";
    case ReadinessState::DEGRADED: return "DEGRADED";
    case ReadinessState::STALE: return "STALE";
    case ReadinessState::INVALIDATED: return "INVALIDATED";
    case ReadinessState::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
    case ReadinessState::MISSING: return "MISSING";
    case ReadinessState::FAILED: return "FAILED";
    case ReadinessState::RECOVERING: return "RECOVERING";
    case ReadinessState::BLOCKED: return "BLOCKED";
    case ReadinessState::SUPERSEDED: return "SUPERSEDED";
    case ReadinessState::RETIRED: return "RETIRED";
  }
  return "UNKNOWN";
}

constexpr std::string_view name_of(GenerationPredicateKind p) {
  switch (p) {
    case GenerationPredicateKind::EXACT: return "EXACT";
    case GenerationPredicateKind::MINIMUM: return "MINIMUM";
    case GenerationPredicateKind::COMPATIBLE_SET: return "COMPATIBLE_SET";
    case GenerationPredicateKind::NONE: return "NONE";
  }
  return "NONE";
}

constexpr std::string_view name_of(SetOperator o) {
  switch (o) {
    case SetOperator::ALL_OF: return "ALL_OF";
    case SetOperator::ANY_OF: return "ANY_OF";
    case SetOperator::AT_LEAST_N: return "AT_LEAST_N";
    case SetOperator::OPTIONAL_GROUP: return "OPTIONAL_GROUP";
  }
  return "ALL_OF";
}

constexpr std::string_view name_of(Outcome o) {
  switch (o) {
    case Outcome::READY: return "READY";
    case Outcome::BLOCKED: return "BLOCKED";
    case Outcome::DEGRADED: return "DEGRADED";
    case Outcome::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
    case Outcome::RECOMPUTE_REQUIRED: return "RECOMPUTE_REQUIRED";
    case Outcome::RECOVERY_REQUIRED: return "RECOVERY_REQUIRED";
    case Outcome::REJECT_STALE_EPOCH: return "REJECT_STALE_EPOCH";
    case Outcome::REJECT_STALE_BOOT: return "REJECT_STALE_BOOT";
    case Outcome::REJECT_STALE_NODE_GENERATION: return "REJECT_STALE_NODE_GENERATION";
    case Outcome::REJECT_STALE_EDGE_GENERATION: return "REJECT_STALE_EDGE_GENERATION";
    case Outcome::REJECT_CYCLE: return "REJECT_CYCLE";
    case Outcome::REJECT_DUPLICATE_ID: return "REJECT_DUPLICATE_ID";
    case Outcome::REJECT_INVALID_TRANSITION: return "REJECT_INVALID_TRANSITION";
    case Outcome::REJECT_UNKNOWN_DEPENDENCY: return "REJECT_UNKNOWN_DEPENDENCY";
    case Outcome::REJECT_MALFORMED: return "REJECT_MALFORMED";
    case Outcome::UNKNOWN: return "UNKNOWN";
    case Outcome::ACCEPTED: return "ACCEPTED";
  }
  return "UNKNOWN";
}

constexpr std::string_view name_of(RecoveryAction a) {
  switch (a) {
    case RecoveryAction::REVALIDATION: return "REVALIDATION";
    case RecoveryAction::RECOMPUTATION: return "RECOMPUTATION";
    case RecoveryAction::RELOAD: return "RELOAD";
    case RecoveryAction::RESTART: return "RESTART";
    case RecoveryAction::REBIND: return "REBIND";
    case RecoveryAction::RESOURCE_REACQUISITION: return "RESOURCE_REACQUISITION";
    case RecoveryAction::CHECKPOINT_RESTORE: return "CHECKPOINT_RESTORE";
    case RecoveryAction::MANUAL_INTERVENTION: return "MANUAL_INTERVENTION";
  }
  return "REVALIDATION";
}

constexpr std::string_view name_of(InvalidationReason r) {
  switch (r) {
    case InvalidationReason::EXPLICIT: return "EXPLICIT";
    case InvalidationReason::UPSTREAM_GONE: return "UPSTREAM_GONE";
    case InvalidationReason::UPSTREAM_GENERATION_CHANGED: return "UPSTREAM_GENERATION_CHANGED";
    case InvalidationReason::COMPATIBILITY_CHANGED: return "COMPATIBILITY_CHANGED";
    case InvalidationReason::RESOURCE_UNAVAILABLE: return "RESOURCE_UNAVAILABLE";
    case InvalidationReason::FRESHNESS_EXPIRED: return "FRESHNESS_EXPIRED";
    case InvalidationReason::INTEGRITY_FAILED: return "INTEGRITY_FAILED";
    case InvalidationReason::AUTHORITY_ADVANCED: return "AUTHORITY_ADVANCED";
    case InvalidationReason::SUPERSEDED: return "SUPERSEDED";
  }
  return "EXPLICIT";
}

constexpr std::string_view name_of(DynamicEvidence e) {
  switch (e) {
    case DynamicEvidence::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
    case DynamicEvidence::STALE: return "STALE";
    case DynamicEvidence::UNKNOWN: return "UNKNOWN";
    case DynamicEvidence::FRESH: return "FRESH";
  }
  return "UNKNOWN";
}

constexpr std::string_view name_of(InvalidationMode m) {
  switch (m) {
    case InvalidationMode::DIRECT: return "DIRECT";
    case InvalidationMode::RECURSIVE: return "RECURSIVE";
    case InvalidationMode::BOUNDED: return "BOUNDED";
    case InvalidationMode::GENERATION: return "GENERATION";
  }
  return "RECURSIVE";
}

constexpr std::string_view name_of(StaleDisposition s) {
  switch (s) {
    case StaleDisposition::REQUIRE_REVALIDATION: return "REQUIRE_REVALIDATION";
    case StaleDisposition::REQUIRE_RESOURCE: return "REQUIRE_RESOURCE";
    case StaleDisposition::REQUIRE_NONE: return "REQUIRE_NONE";
  }
  return "REQUIRE_REVALIDATION";
}

// Parsers (case-sensitive, exact).
std::optional<NodeKind> node_kind_from_name(std::string_view s);
std::optional<EdgeKind> edge_kind_from_name(std::string_view s);
std::optional<ReadinessState> readiness_state_from_name(std::string_view s);
std::optional<Outcome> outcome_from_name(std::string_view s);
std::optional<RecoveryAction> recovery_action_from_name(std::string_view s);
std::optional<InvalidationReason> invalidation_reason_from_name(std::string_view s);
std::optional<SetOperator> set_operator_from_name(std::string_view s);
std::optional<GenerationPredicateKind> generation_predicate_from_name(std::string_view s);

}  // namespace dependency_fabric
