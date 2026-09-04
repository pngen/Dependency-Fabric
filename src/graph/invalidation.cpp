// dependency_fabric::graph — invalidation, supersession, and stale authority.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dependency_fabric/graph.hpp"

#include <algorithm>
#include <deque>
#include <unordered_set>

namespace dependency_fabric {

namespace {
template <typename T>
T bump(const T& v) { return v.valid() ? T(v.value() + 1) : T(1); }
}  // namespace

void Graph::propagate_invalidation_locked(DependencyNodeId seed, const InvalidationPolicy& policy,
                                          InvalidationReason reason, std::uint32_t depth_remaining) {
  (void)depth_remaining;
  std::unordered_set<std::uint64_t> visited;
  std::deque<std::pair<DependencyNodeId, std::uint32_t>> q;
  q.push_back({seed, 0});
  visited.insert(seed.value());

  const bool bounded = (policy.mode == InvalidationMode::BOUNDED);
  const std::uint32_t max_depth = policy.max_depth != 0 ? policy.max_depth : kDefaultMaxTraversalDepth;

  while (!q.empty()) {
    auto [cur, depth] = q.front();
    q.pop_front();
    auto it = out_edges_.find(cur);
    if (it == out_edges_.end()) continue;
    for (auto eid : it->second) {
      auto eit = edges_.find(eid);
      if (eit == edges_.end() || !eit->second.active) continue;
      const Edge& e = eit->second;
      if (e.kind == EdgeKind::OPTIONAL && !policy.propagate_through_optional) {
        // Optional edge does not propagate the invalidation event.
        continue;
      }
      if (bounded && depth + 1 > max_depth) continue;
      auto nxt = e.consumer_id;
      if (!visited.insert(nxt.value()).second) continue;
      Node* n = find_locked(nxt);
      if (n) {
        n->ever_invalidated = true;
        n->last_invalidation_reason = reason;
        n->invalidation_generation = bump(n->invalidation_generation);
      }
      q.push_back({nxt, depth + 1});
    }
  }
}

Outcome Graph::invalidate_node(DependencyNodeId id, InvalidationReason reason,
                               const InvalidationPolicy& policy,
                               SourceId source, SourceBootId boot, CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  Node* n = find_locked(id);
  if (!n) return Outcome::REJECT_UNKNOWN_DEPENDENCY;

  n->state = ReadinessState::INVALIDATED;
  n->ever_invalidated = true;
  n->last_invalidation_reason = reason;
  n->invalidation_generation = bump(n->invalidation_generation);

  if (policy.mode != InvalidationMode::DIRECT) {
    const std::uint32_t depth = (policy.mode == InvalidationMode::BOUNDED)
                                    ? (policy.max_depth != 0 ? policy.max_depth : kDefaultMaxTraversalDepth)
                                    : kDefaultMaxTraversalDepth;
    propagate_invalidation_locked(id, policy, reason, depth);
  }

  // Recompute the seed and its dependents so states become BLOCKED/DEGRADED/STALE
  // according to policy.
  std::vector<DependencyNodeId> roots{id};
  recompute_closure_locked(roots);
  return Outcome::ACCEPTED;
}

Outcome Graph::invalidate_generation(DependencyNodeId id, DependencyNodeGeneration gen,
                                     InvalidationReason reason, const InvalidationPolicy& policy,
                                     SourceId source, SourceBootId boot, CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  Node* n = find_locked(id);
  if (!n) return Outcome::REJECT_UNKNOWN_DEPENDENCY;
  if (!gen.valid()) return Outcome::REJECT_INVALID_TRANSITION;
  if (!n->has_generation(gen)) return Outcome::REJECT_STALE_NODE_GENERATION;

  if (gen.value() == n->current_generation.value()) {
    // Invalidating the current authoritative generation invalidates the node.
    n->state = ReadinessState::INVALIDATED;
    n->ever_invalidated = true;
    n->last_invalidation_reason = reason;
    n->invalidation_generation = bump(n->invalidation_generation);
    if (policy.mode != InvalidationMode::DIRECT) {
      const std::uint32_t depth = (policy.mode == InvalidationMode::BOUNDED)
                                      ? (policy.max_depth != 0 ? policy.max_depth : kDefaultMaxTraversalDepth)
                                      : kDefaultMaxTraversalDepth;
      propagate_invalidation_locked(id, policy, reason, depth);
    }
  } else {
    // Invalidating a historical generation: mark it superseded so that
    // dependents bound to it become stale.
    if (std::find(n->superseded_generations.begin(), n->superseded_generations.end(), gen) ==
        n->superseded_generations.end()) {
      n->superseded_generations.push_back(gen);
    }
    n->ever_invalidated = true;
    n->last_invalidation_reason = reason;
  }
  std::vector<DependencyNodeId> roots{id};
  recompute_closure_locked(roots);
  return Outcome::ACCEPTED;
}

Outcome Graph::mark_stale(DependencyNodeId id, SourceId source, SourceBootId boot,
                          CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  Node* n = find_locked(id);
  if (!n) return Outcome::REJECT_UNKNOWN_DEPENDENCY;
  n->state = ReadinessState::STALE;
  n->recovered_evidence = DynamicEvidence::STALE;
  std::vector<DependencyNodeId> roots{id};
  recompute_closure_locked(roots);
  return Outcome::ACCEPTED;
}

Outcome Graph::mark_missing(DependencyNodeId id, SourceId source, SourceBootId boot,
                            CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  Node* n = find_locked(id);
  if (!n) return Outcome::REJECT_UNKNOWN_DEPENDENCY;
  n->state = ReadinessState::MISSING;
  std::vector<DependencyNodeId> roots{id};
  recompute_closure_locked(roots);
  return Outcome::ACCEPTED;
}

Outcome Graph::mark_recovered(DependencyNodeId id, SourceId source, SourceBootId boot,
                              CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  Node* n = find_locked(id);
  if (!n) return Outcome::REJECT_UNKNOWN_DEPENDENCY;
  n->ever_invalidated = false;
  // Recovery must not silently revalidate dynamic evidence.
  if (n->dynamic_evidence) {
    n->recovered_evidence = DynamicEvidence::REVALIDATION_REQUIRED;
    n->state = ReadinessState::REVALIDATION_REQUIRED;
  } else {
    n->recovered_evidence = DynamicEvidence::FRESH;
    n->state = ReadinessState::PRESENT;
  }
  std::vector<DependencyNodeId> roots{id};
  recompute_closure_locked(roots);
  return Outcome::ACCEPTED;
}

}  // namespace dependency_fabric