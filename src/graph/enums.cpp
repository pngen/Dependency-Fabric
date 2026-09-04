// dependency_fabric::enums — parser implementations.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dependency_fabric/enums.hpp"

namespace dependency_fabric {

namespace {
template <typename T>
std::optional<T> parse(const std::string_view s, const std::pair<std::string_view, T>* table, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    if (table[i].first == s) return table[i].second;
  }
  return std::nullopt;
}
}  // namespace

std::optional<NodeKind> node_kind_from_name(std::string_view s) {
  static const std::pair<std::string_view, NodeKind> table[] = {
      {"DATA", NodeKind::DATA}, {"TENSOR", NodeKind::TENSOR}, {"KV_STATE", NodeKind::KV_STATE},
      {"CHECKPOINT", NodeKind::CHECKPOINT}, {"MODEL", NodeKind::MODEL}, {"ADAPTER", NodeKind::ADAPTER},
      {"KERNEL", NodeKind::KERNEL}, {"EXECUTION_GRAPH", NodeKind::EXECUTION_GRAPH},
      {"ARTIFACT", NodeKind::ARTIFACT}, {"SERVICE", NodeKind::SERVICE}, {"RESOURCE", NodeKind::RESOURCE},
      {"TOPOLOGY", NodeKind::TOPOLOGY}, {"CAPABILITY", NodeKind::CAPABILITY},
      {"EXECUTION_RESULT", NodeKind::EXECUTION_RESULT}, {"WORKLOAD_PHASE", NodeKind::WORKLOAD_PHASE},
      {"POLICY", NodeKind::POLICY}, {"UNKNOWN", NodeKind::UNKNOWN}};
  return parse(s, table, sizeof(table) / sizeof(table[0]));
}

std::optional<EdgeKind> edge_kind_from_name(std::string_view s) {
  static const std::pair<std::string_view, EdgeKind> table[] = {
      {"REQUIRES", EdgeKind::REQUIRES}, {"OPTIONAL", EdgeKind::OPTIONAL},
      {"PRODUCES", EdgeKind::PRODUCES}, {"DERIVES_FROM", EdgeKind::DERIVES_FROM},
      {"INVALIDATES_WITH", EdgeKind::INVALIDATES_WITH}, {"COMPATIBLE_WITH", EdgeKind::COMPATIBLE_WITH},
      {"REQUIRES_RESOURCE", EdgeKind::REQUIRES_RESOURCE},
      {"REQUIRES_CAPABILITY", EdgeKind::REQUIRES_CAPABILITY},
      {"REQUIRES_READY", EdgeKind::REQUIRES_READY}, {"REQUIRES_FRESH", EdgeKind::REQUIRES_FRESH},
      {"REQUIRES_GENERATION", EdgeKind::REQUIRES_GENERATION}};
  return parse(s, table, sizeof(table) / sizeof(table[0]));
}

std::optional<ReadinessState> readiness_state_from_name(std::string_view s) {
  static const std::pair<std::string_view, ReadinessState> table[] = {
      {"UNKNOWN", ReadinessState::UNKNOWN}, {"DECLARED", ReadinessState::DECLARED},
      {"DISCOVERED", ReadinessState::DISCOVERED}, {"PRESENT", ReadinessState::PRESENT},
      {"VALIDATING", ReadinessState::VALIDATING}, {"READY", ReadinessState::READY},
      {"DEGRADED", ReadinessState::DEGRADED}, {"STALE", ReadinessState::STALE},
      {"INVALIDATED", ReadinessState::INVALIDATED},
      {"REVALIDATION_REQUIRED", ReadinessState::REVALIDATION_REQUIRED},
      {"MISSING", ReadinessState::MISSING}, {"FAILED", ReadinessState::FAILED},
      {"RECOVERING", ReadinessState::RECOVERING}, {"BLOCKED", ReadinessState::BLOCKED},
      {"SUPERSEDED", ReadinessState::SUPERSEDED}, {"RETIRED", ReadinessState::RETIRED}};
  return parse(s, table, sizeof(table) / sizeof(table[0]));
}

std::optional<Outcome> outcome_from_name(std::string_view s) {
  static const std::pair<std::string_view, Outcome> table[] = {
      {"READY", Outcome::READY}, {"BLOCKED", Outcome::BLOCKED}, {"DEGRADED", Outcome::DEGRADED},
      {"REVALIDATION_REQUIRED", Outcome::REVALIDATION_REQUIRED},
      {"RECOMPUTE_REQUIRED", Outcome::RECOMPUTE_REQUIRED},
      {"RECOVERY_REQUIRED", Outcome::RECOVERY_REQUIRED},
      {"REJECT_STALE_EPOCH", Outcome::REJECT_STALE_EPOCH},
      {"REJECT_STALE_BOOT", Outcome::REJECT_STALE_BOOT},
      {"REJECT_STALE_NODE_GENERATION", Outcome::REJECT_STALE_NODE_GENERATION},
      {"REJECT_STALE_EDGE_GENERATION", Outcome::REJECT_STALE_EDGE_GENERATION},
      {"REJECT_CYCLE", Outcome::REJECT_CYCLE}, {"REJECT_DUPLICATE_ID", Outcome::REJECT_DUPLICATE_ID},
      {"REJECT_INVALID_TRANSITION", Outcome::REJECT_INVALID_TRANSITION},
      {"REJECT_UNKNOWN_DEPENDENCY", Outcome::REJECT_UNKNOWN_DEPENDENCY},
      {"REJECT_MALFORMED", Outcome::REJECT_MALFORMED}, {"UNKNOWN", Outcome::UNKNOWN},
      {"ACCEPTED", Outcome::ACCEPTED}};
  return parse(s, table, sizeof(table) / sizeof(table[0]));
}

std::optional<RecoveryAction> recovery_action_from_name(std::string_view s) {
  static const std::pair<std::string_view, RecoveryAction> table[] = {
      {"REVALIDATION", RecoveryAction::REVALIDATION}, {"RECOMPUTATION", RecoveryAction::RECOMPUTATION},
      {"RELOAD", RecoveryAction::RELOAD}, {"RESTART", RecoveryAction::RESTART},
      {"REBIND", RecoveryAction::REBIND},
      {"RESOURCE_REACQUISITION", RecoveryAction::RESOURCE_REACQUISITION},
      {"CHECKPOINT_RESTORE", RecoveryAction::CHECKPOINT_RESTORE},
      {"MANUAL_INTERVENTION", RecoveryAction::MANUAL_INTERVENTION}};
  return parse(s, table, sizeof(table) / sizeof(table[0]));
}

std::optional<InvalidationReason> invalidation_reason_from_name(std::string_view s) {
  static const std::pair<std::string_view, InvalidationReason> table[] = {
      {"EXPLICIT", InvalidationReason::EXPLICIT}, {"UPSTREAM_GONE", InvalidationReason::UPSTREAM_GONE},
      {"UPSTREAM_GENERATION_CHANGED", InvalidationReason::UPSTREAM_GENERATION_CHANGED},
      {"COMPATIBILITY_CHANGED", InvalidationReason::COMPATIBILITY_CHANGED},
      {"RESOURCE_UNAVAILABLE", InvalidationReason::RESOURCE_UNAVAILABLE},
      {"FRESHNESS_EXPIRED", InvalidationReason::FRESHNESS_EXPIRED},
      {"INTEGRITY_FAILED", InvalidationReason::INTEGRITY_FAILED},
      {"AUTHORITY_ADVANCED", InvalidationReason::AUTHORITY_ADVANCED},
      {"SUPERSEDED", InvalidationReason::SUPERSEDED}};
  return parse(s, table, sizeof(table) / sizeof(table[0]));
}

std::optional<SetOperator> set_operator_from_name(std::string_view s) {
  static const std::pair<std::string_view, SetOperator> table[] = {
      {"ALL_OF", SetOperator::ALL_OF}, {"ANY_OF", SetOperator::ANY_OF},
      {"AT_LEAST_N", SetOperator::AT_LEAST_N}, {"OPTIONAL_GROUP", SetOperator::OPTIONAL_GROUP}};
  return parse(s, table, sizeof(table) / sizeof(table[0]));
}

std::optional<GenerationPredicateKind> generation_predicate_from_name(std::string_view s) {
  static const std::pair<std::string_view, GenerationPredicateKind> table[] = {
      {"EXACT", GenerationPredicateKind::EXACT}, {"MINIMUM", GenerationPredicateKind::MINIMUM},
      {"COMPATIBLE_SET", GenerationPredicateKind::COMPATIBLE_SET},
      {"NONE", GenerationPredicateKind::NONE}};
  return parse(s, table, sizeof(table) / sizeof(table[0]));
}

}  // namespace dependency_fabric
