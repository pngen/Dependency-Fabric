// dependency_fabric::graph — readiness evaluation, explanations, and blockers.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dependency_fabric/graph.hpp"

#include <algorithm>
#include <deque>
#include <unordered_set>

namespace dependency_fabric {

namespace {

// A coarse ranking used to compare "required readiness level" vs actual state.
// A state satisfies a requirement when its rank is at least the requirement's
// rank. READY is the only state that satisfies a READY requirement.
template <typename T>
T next_counter(const T& v) { return v.valid() ? T(v.value() + 1) : T(1); }

bool is_gate_kind(NodeKind k) {
  return k == NodeKind::WORKLOAD_PHASE || k == NodeKind::SERVICE;
}

bool requires_own_generation(NodeKind k) {
  switch (k) {
    case NodeKind::WORKLOAD_PHASE: return false;
    default: return true;
  }
}

int readiness_rank(ReadinessState s) {
  switch (s) {
    case ReadinessState::READY: return 100;
    case ReadinessState::PRESENT: return 80;
    case ReadinessState::VALIDATING: return 70;
    case ReadinessState::DEGRADED: return 60;
    case ReadinessState::DISCOVERED: return 50;
    case ReadinessState::DECLARED: return 40;
    case ReadinessState::RECOVERING: return 30;
    case ReadinessState::STALE: return 20;
    case ReadinessState::REVALIDATION_REQUIRED: return 16;
    case ReadinessState::SUPERSEDED: return 14;
    case ReadinessState::INVALIDATED: return 12;
    case ReadinessState::BLOCKED: return 10;
    case ReadinessState::MISSING: return 5;
    case ReadinessState::FAILED: return 3;
    case ReadinessState::UNKNOWN: return 1;
    case ReadinessState::RETIRED: return 0;
  }
  return 1;
}

std::uint64_t hmax(const std::vector<DependencyNodeGeneration>& v) {
  std::uint64_t m = 0;
  for (auto& g : v) m = std::max(m, g.value());
  return m;
}

Outcome outcome_for_state(ReadinessState s) {
  switch (s) {
    case ReadinessState::READY: return Outcome::READY;
    case ReadinessState::DEGRADED: return Outcome::DEGRADED;
    case ReadinessState::STALE: return Outcome::RECOMPUTE_REQUIRED;
    case ReadinessState::INVALIDATED: return Outcome::RECOVERY_REQUIRED;
    case ReadinessState::REVALIDATION_REQUIRED: return Outcome::REVALIDATION_REQUIRED;
    case ReadinessState::MISSING: return Outcome::RECOVERY_REQUIRED;
    case ReadinessState::FAILED: return Outcome::RECOVERY_REQUIRED;
    case ReadinessState::RECOVERING: return Outcome::RECOVERY_REQUIRED;
    case ReadinessState::BLOCKED: return Outcome::BLOCKED;
    case ReadinessState::SUPERSEDED: return Outcome::REVALIDATION_REQUIRED;
    case ReadinessState::DECLARED: return Outcome::REVALIDATION_REQUIRED;
    case ReadinessState::PRESENT: return Outcome::REVALIDATION_REQUIRED;
    case ReadinessState::DISCOVERED: return Outcome::REVALIDATION_REQUIRED;
    case ReadinessState::VALIDATING: return Outcome::REVALIDATION_REQUIRED;
    case ReadinessState::UNKNOWN: return Outcome::UNKNOWN;
    case ReadinessState::RETIRED: return Outcome::UNKNOWN;
  }
  return Outcome::UNKNOWN;
}

}  // namespace

bool Graph::meets_readiness_constraint(ReadinessState actual, ReadinessState required) const {
  return readiness_rank(actual) >= readiness_rank(required);
}

bool Graph::meets_generation_constraint(const Node& producer, const DependencyPredicate& pred,
                                        DependencyNodeGeneration& required,
                                        GenerationPredicateKind& kind) const {
  switch (pred.generation_kind) {
    case GenerationPredicateKind::NONE:
      kind = GenerationPredicateKind::NONE;
      return true;
    case GenerationPredicateKind::EXACT:
      kind = GenerationPredicateKind::EXACT;
      required = pred.exact_generation;
      return producer.current_generation.valid() &&
             producer.current_generation.value() == pred.exact_generation.value();
    case GenerationPredicateKind::MINIMUM:
      kind = GenerationPredicateKind::MINIMUM;
      required = pred.minimum_generation;
      return producer.current_generation.valid() &&
             producer.current_generation.value() >= pred.minimum_generation.value();
    case GenerationPredicateKind::COMPATIBLE_SET:
      kind = GenerationPredicateKind::COMPATIBLE_SET;
      if (!producer.current_generation.valid()) return false;
      for (auto& g : pred.compatible_generations) {
        if (g.value() == producer.current_generation.value()) return true;
      }
      required = producer.current_generation;
      return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Per-edge and per-set predicate evaluation (const, read-only).
// ---------------------------------------------------------------------------
Outcome Graph::eval_edge_locked(const Edge& edge, std::vector<BlockingDependency>& blockers,
                                bool& blocked, bool& degraded, bool& stale) const {
  const Node* p = find_locked(edge.producer_id);
  const bool req = edge.required();

  auto add_blocker = [&](std::string reason, GenerationPredicateKind kind,
                         DependencyNodeGeneration reqg, DependencyNodeGeneration actual) {
    BlockingDependency b;
    b.edge_id = edge.id;
    b.edge_generation = edge.generation;
    b.producer = edge.producer_id;
    b.producer_kind = p ? p->kind : NodeKind::UNKNOWN;
    b.kind = edge.kind;
    b.optional = !req;
    b.constraint_kind = kind;
    b.required_generation = reqg;
    b.actual_generation = actual;
    b.reason = std::move(reason);
    blockers.push_back(std::move(b));
  };

  if (!p) {
    if (req) { blocked = true; add_blocker("prerequisite node is missing/unknown", GenerationPredicateKind::NONE, {}, {}); return Outcome::BLOCKED; }
    degraded = true; add_blocker("optional prerequisite is missing/unknown", GenerationPredicateKind::NONE, {}, {}); return Outcome::DEGRADED;
  }

  // Generation constraint.
  DependencyNodeGeneration reqg;
  GenerationPredicateKind kind = GenerationPredicateKind::NONE;
  const bool gen_ok = meets_generation_constraint(*p, edge.predicate, reqg, kind);
  if (!gen_ok) {
    bool advanced = false;
    if (edge.predicate.generation_kind == GenerationPredicateKind::EXACT &&
        p->current_generation.valid() &&
        p->current_generation.value() > edge.predicate.exact_generation.value()) {
      advanced = true;
    } else if (edge.predicate.generation_kind == GenerationPredicateKind::COMPATIBLE_SET &&
               !edge.predicate.compatible_generations.empty() &&
               p->current_generation.valid() &&
               p->current_generation.value() > hmax(edge.predicate.compatible_generations)) {
      advanced = true;
    }
    if (advanced) {
      // The producer advanced beyond what this consumer was built against:
      // the consumer is STALE, not merely blocked.
      stale = true;
      add_blocker("producer generation advanced beyond pinned dependency generation; node is STALE",
                  kind, reqg, p->current_generation);
      // A stale constraint still prevents readiness; mark degraded so the
      // node is never treated as fully ready while stale.
      degraded = true;
    } else {
      if (req) {
        blocked = true;
        add_blocker("producer generation does not satisfy required generation constraint (not yet available)",
                    kind, reqg, p->current_generation);
      } else {
        degraded = true;
        add_blocker("optional producer generation does not satisfy generation constraint",
                    kind, reqg, p->current_generation);
      }
    }
  }

  // Readiness level requirement. A stale/superseded prerequisite propagates
  // staleness to the consumer (the consumer is built on a stale lineage); a
  // hard-blocking prerequisite (BLOCKED/INVALIDATED/MISSING/FAILED) blocks.
  if (!meets_readiness_constraint(p->state, edge.predicate.required_readiness)) {
    std::string reason = "producer readiness " + std::string(name_of(p->state)) +
                         " does not meet required " + std::string(name_of(edge.predicate.required_readiness));
    if (req) {
      if (p->state == ReadinessState::STALE || p->state == ReadinessState::SUPERSEDED) {
        stale = true;
        add_blocker(std::move(reason), GenerationPredicateKind::NONE, {}, {});
      } else {
        blocked = true;
        add_blocker(std::move(reason), GenerationPredicateKind::NONE, {}, {});
      }
    } else {
      degraded = true;
      add_blocker(std::move(reason), GenerationPredicateKind::NONE, {}, {});
    }
  }

  // Resource availability.
  if (edge.predicate.requires_resource_available) {
    const bool avail = adapter_ ? adapter_->resource_available(edge.producer_id) : p->resource_available;
    if (!avail) {
      if (req) { blocked = true; add_blocker("required resource is not available", GenerationPredicateKind::NONE, {}, {}); }
      else { degraded = true; add_blocker("optional resource is not available", GenerationPredicateKind::NONE, {}, {}); }
    }
  }

  // Capability requirement.
  if (!edge.predicate.required_capability.empty()) {
    bool claimed = false;
    if (adapter_) {
      claimed = adapter_->capability_claimed(edge.producer_id, edge.predicate.required_capability);
    } else {
      for (const auto& c : p->claimed_capabilities) {
        if (c == edge.predicate.required_capability) { claimed = true; break; }
      }
    }
    if (!claimed) {
      if (req) { blocked = true; add_blocker("required capability '" + edge.predicate.required_capability + "' is not claimed", GenerationPredicateKind::NONE, {}, {}); }
      else { degraded = true; add_blocker("optional capability '" + edge.predicate.required_capability + "' is not claimed", GenerationPredicateKind::NONE, {}, {}); }
    }
  }

  // Freshness.
  if (edge.predicate.requires_fresh) {
    const bool ok = adapter_ ? adapter_->freshness_ok(edge.producer_id) : p->freshness_valid;
    if (!ok) {
      if (req) { blocked = true; add_blocker("producer freshness has expired", GenerationPredicateKind::NONE, {}, {}); }
      else { degraded = true; add_blocker("optional producer freshness has expired", GenerationPredicateKind::NONE, {}, {}); }
    }
  }

  // Integrity.
  if (edge.predicate.requires_integrity) {
    const bool ok = adapter_ ? adapter_->integrity_ok(edge.producer_id) : p->integrity_valid;
    if (!ok) {
      if (req) { blocked = true; add_blocker("producer integrity validation failed", GenerationPredicateKind::NONE, {}, {}); }
      else { degraded = true; add_blocker("optional producer integrity validation failed", GenerationPredicateKind::NONE, {}, {}); }
    }
  }

  // Provenance confidence.
  if (edge.predicate.minimal_provenance_confidence > 0) {
    const auto conf = adapter_ ? adapter_->provenance_confidence(edge.producer_id) : p->provenance_confidence;
    if (conf < edge.predicate.minimal_provenance_confidence) {
      if (req) { blocked = true; add_blocker("provenance confidence below required threshold", GenerationPredicateKind::NONE, {}, {}); }
      else { degraded = true; add_blocker("optional provenance confidence below required threshold", GenerationPredicateKind::NONE, {}, {}); }
    }
  }

  // Completion state.
  if (!edge.predicate.required_completion_state.empty()) {
    const auto cs = adapter_ ? std::string(adapter_->completion_state(edge.producer_id)) : p->completion_state;
    if (cs != edge.predicate.required_completion_state) {
      if (req) { blocked = true; add_blocker("producer completion state does not match required", GenerationPredicateKind::NONE, {}, {}); }
      else { degraded = true; add_blocker("optional producer completion state does not match required", GenerationPredicateKind::NONE, {}, {}); }
    }
  }

  if (blocked) return Outcome::BLOCKED;
  if (degraded) return Outcome::DEGRADED;
  return Outcome::READY;
}

Outcome Graph::eval_set_locked(const DependencySet& set, std::vector<BlockingDependency>& blockers,
                               bool& blocked, bool& degraded, bool& stale,
                               std::uint32_t& satisfied) const {
  std::uint32_t total = 0;
  std::uint32_t ok_count = 0;
  const std::size_t start = blockers.size();
  bool any_optional_fail = false;
  bool any_failed = false;

  for (auto eid : set.members) {
    const Edge* e = find_edge_locked(eid);
    if (!e || !e->active) continue;
    ++total;
    bool edge_blocked = false, edge_degraded = false, edge_stale = false;
    std::vector<BlockingDependency> local;
    eval_edge_locked(*e, local, edge_blocked, edge_degraded, edge_stale);
    for (auto& b : local) b.is_set_member = true;
    blockers.insert(blockers.end(), local.begin(), local.end());
    if (edge_blocked) {
      any_failed = true;
    } else if (edge_degraded && e->required()) {
      // a required edge that is only degraded (stale) still counts as satisfied
      // only for the purpose of count; staleness is tracked separately.
      ++ok_count;
      if (edge_stale) stale = true;
      if (!e->required()) any_optional_fail = true;
    } else if (edge_degraded && !e->required()) {
      any_optional_fail = true;
    } else {
      ++ok_count;
      if (edge_stale) stale = true;
    }
  }

  // Evaluate the set operator.
  bool satisfied_set = true;
  switch (set.op) {
    case SetOperator::ALL_OF: {
      // All required members must be satisfied. Sat test uses ok_count: an
      // edge that degraded (stale) is still counted. But a stale required edge
      // must not make the set "satisfied" for readiness; we detect staleness
      // via 'stale'. For ALL_OF we require every member that was evaluated to
      // have passed (ok_count == total) unless it was a pure optional degrade.
      satisfied_set = (ok_count == total);
      break;
    }
    case SetOperator::ANY_OF:
      satisfied_set = (ok_count >= 1);
      break;
    case SetOperator::AT_LEAST_N:
      satisfied_set = (ok_count >= set.at_least_n);
      break;
    case SetOperator::OPTIONAL_GROUP:
      // never hard-blocks; group is optional
      satisfied_set = true;
      break;
  }

  satisfied = ok_count;

  // If the set is satisfied, drop the per-member blockers we appended (they are
  // informational only and do not block the group). A satisfied OPTIONAL_GROUP
  // with failed members degrades the consumer instead of blocking it.
  if (satisfied_set) {
    blockers.resize(start);
    if (any_optional_fail || stale) degraded = true;
    if (set.op == SetOperator::OPTIONAL_GROUP && any_failed) degraded = true;
    return Outcome::READY;
  }

  // The set is NOT satisfied -> it blocks the consumer.
  blocked = true;
  return Outcome::BLOCKED;
}

// ---------------------------------------------------------------------------
// Recompute a single node's derived state.
// ---------------------------------------------------------------------------
Outcome Graph::recompute_node_locked(DependencyNodeId id) {
  Node* n = find_locked(id);
  if (!n) return Outcome::REJECT_UNKNOWN_DEPENDENCY;

  // Maintenance escape states are preserved (in-flight / human states).
  switch (n->state) {
    case ReadinessState::RETIRED:
    case ReadinessState::MISSING:
    case ReadinessState::FAILED:
    case ReadinessState::RECOVERING:
      n->readiness_generation = next_counter(n->readiness_generation);
      return Outcome::ACCEPTED;
    default:
      break;
  }

  // Directly invalidated nodes stay invalidated until explicitly recovered.
  if (n->state == ReadinessState::INVALIDATED) {
    n->readiness_generation = next_counter(n->readiness_generation);
    return Outcome::ACCEPTED;
  }

  // Revalidation gate: dynamic evidence that has not been re-corroborated is
  // conservatively non-ready after recovery/restart.
  if (n->dynamic_evidence && n->recovered_evidence == DynamicEvidence::REVALIDATION_REQUIRED) {
    if (n->state != ReadinessState::REVALIDATION_REQUIRED) {
      n->state = ReadinessState::REVALIDATION_REQUIRED;
      n->readiness_generation = next_counter(n->readiness_generation);
    }
    return Outcome::REVALIDATION_REQUIRED;
  }

  bool blocked = false, degraded = false, stale = false;
  std::vector<BlockingDependency> acc;

  // Evaluate each dependency set of this node.
  auto sit = node_sets_.find(id);
  std::vector<DependencyEdgeId> set_members;
  if (sit != node_sets_.end()) {
    for (auto sid : sit->second) {
      auto it = sets_.find(sid);
      if (it == sets_.end() || !it->second.active) continue;
      set_members.insert(set_members.end(), it->second.members.begin(), it->second.members.end());
      std::uint32_t sat = 0;
      const Outcome so = eval_set_locked(it->second, acc, blocked, degraded, stale, sat);
      (void)so;
    }
  }

  // Edges not members of any set form an implicit ALL_OF group.
  auto iit = in_edges_.find(id);
  if (iit != in_edges_.end()) {
    for (auto eid : iit->second) {
      if (std::find(set_members.begin(), set_members.end(), eid) != set_members.end()) continue;
      const Edge* e = find_edge_locked(eid);
      if (!e || !e->active) continue;
      eval_edge_locked(*e, acc, blocked, degraded, stale);
    }
  }

  ReadinessState derived;
  if (blocked) {
    derived = ReadinessState::BLOCKED;
  } else if (stale) {
    derived = is_gate_kind(n->kind) ? ReadinessState::BLOCKED : ReadinessState::STALE;
  } else if (degraded) {
    derived = ReadinessState::DEGRADED;
  } else if (requires_own_generation(n->kind) && !n->current_generation.valid()) {
    derived = ReadinessState::DECLARED;
  } else {
    derived = ReadinessState::READY;
  }

  if (n->state != derived) {
    n->state = derived;
    n->readiness_generation = next_counter(n->readiness_generation);
  }
  return outcome_for_state(derived);
}

void Graph::recompute_all_locked() {
  // Kahn topological order over the whole graph (producers first).
  std::unordered_map<DependencyNodeId, std::uint32_t> indeg;
  for (const auto& kv : nodes_) indeg[kv.first] = 0;
  for (const auto& kv : edges_) {
    if (!kv.second.active) continue;
    indeg[kv.second.consumer_id]++;
  }
  std::deque<DependencyNodeId> q;
  for (const auto& kv : indeg) if (kv.second == 0) q.push_back(kv.first);
  std::vector<DependencyNodeId> order;
  while (!q.empty()) {
    auto cur = q.front(); q.pop_front();
    order.push_back(cur);
    auto it = out_edges_.find(cur);
    if (it == out_edges_.end()) continue;
    for (auto eid : it->second) {
      auto eit = edges_.find(eid);
      if (eit == edges_.end() || !eit->second.active) continue;
      auto c = eit->second.consumer_id;
      if (--indeg[c] == 0) q.push_back(c);
    }
  }
  // Remaining (should not happen in an acyclic graph) processed by id order.
  for (auto& kv : indeg) {
    if (std::find(order.begin(), order.end(), kv.first) == order.end()) order.push_back(kv.first);
  }
  for (auto id : order) recompute_node_locked(id);
}

void Graph::recompute_closure_locked(std::vector<DependencyNodeId> roots) {
  std::unordered_set<std::uint64_t> seen;
  std::deque<DependencyNodeId> q;
  for (auto& r : roots) {
    if (r.valid() && seen.insert(r.value()).second) q.push_back(r);
  }
  std::vector<DependencyNodeId> order;
  while (!q.empty()) {
    auto cur = q.front(); q.pop_front();
    order.push_back(cur);
    auto it = out_edges_.find(cur);
    if (it == out_edges_.end()) continue;
    for (auto eid : it->second) {
      auto eit = edges_.find(eid);
      if (eit == edges_.end() || !eit->second.active) continue;
      auto nxt = eit->second.consumer_id;
      if (seen.insert(nxt.value()).second) q.push_back(nxt);
    }
  }
  for (auto id : order) recompute_node_locked(id);
}

Outcome Graph::recompute_readiness(DependencyNodeId id) {
  std::unique_lock lock(mutex_);
  return recompute_node_locked(id);
}

Outcome Graph::recompute_all_readiness() {
  std::unique_lock lock(mutex_);
  recompute_all_locked();
  return Outcome::ACCEPTED;
}

// ---------------------------------------------------------------------------
// Explanations and blockers.
// ---------------------------------------------------------------------------
std::vector<BlockingDependency> Graph::blockers(DependencyNodeId id) const {
  std::shared_lock lock(mutex_);
  std::vector<BlockingDependency> result;
  const Node* n = find_locked(id);
  if (!n) return result;

  auto sit = node_sets_.find(id);
  if (sit != node_sets_.end()) {
    for (auto sid : sit->second) {
      auto it = sets_.find(sid);
      if (it == sets_.end() || !it->second.active) continue;
      bool b = false, d = false, st = false; std::uint32_t sat = 0;
      std::vector<BlockingDependency> tmp;
      eval_set_locked(it->second, tmp, b, d, st, sat);
      if (b) {
        for (auto& x : tmp) if (!x.optional) result.push_back(std::move(x));
      }
    }
  }

  std::vector<DependencyEdgeId> set_members;
  if (sit != node_sets_.end()) {
    for (auto sid : sit->second) {
      auto it = sets_.find(sid);
      if (it != sets_.end()) {
        set_members.insert(set_members.end(), it->second.members.begin(), it->second.members.end());
      }
    }
  }
  auto iit = in_edges_.find(id);
  if (iit != in_edges_.end()) {
    for (auto eid : iit->second) {
      if (std::find(set_members.begin(), set_members.end(), eid) != set_members.end()) continue;
      const Edge* e = find_edge_locked(eid);
      if (!e || !e->active) continue;
      bool b = false, d = false, st = false;
      std::vector<BlockingDependency> tmp;
      eval_edge_locked(*e, tmp, b, d, st);
      if (b || st) {
        for (auto& x : tmp) if (!x.optional) result.push_back(std::move(x));
      }
    }
  }
  return result;
}

Explanation Graph::explain(DependencyNodeId id) const {
  std::shared_lock lock(mutex_);
  Explanation ex;
  const Node* n = find_locked(id);
  if (!n) {
    ex.node = id;
    ex.state = ReadinessState::UNKNOWN;
    ex.outcome = Outcome::UNKNOWN;
    ex.summary = "node does not exist (UNKNOWN)";
    return ex;
  }
  ex.node = id;
  ex.kind = n->kind;
  ex.state = n->state;
  ex.outcome = outcome_for_state(n->state);
  ex.current_generation = n->current_generation;
  ex.origin_source = n->origin_source;
  ex.origin_boot = n->origin_boot;
  ex.summary = "node " + std::to_string(id.value()) + " (" + std::string(name_of(n->kind)) +
               ") is " + std::string(name_of(n->state));

  // Lock re-acquire would deadlock under shared lock; compute blockers without
  // re-locking using the same internal evaluation.
  auto sit = node_sets_.find(id);
  std::vector<DependencyEdgeId> set_members;
  if (sit != node_sets_.end()) {
    for (auto sid : sit->second) {
      auto it = sets_.find(sid);
      if (it == sets_.end() || !it->second.active) continue;
      set_members.insert(set_members.end(), it->second.members.begin(), it->second.members.end());
      bool b = false, d = false, st = false; std::uint32_t sat = 0;
      std::vector<BlockingDependency> tmp;
      eval_set_locked(it->second, tmp, b, d, st, sat);
      if (b) for (auto& x : tmp) if (!x.optional) ex.blockers.push_back(std::move(x));
    }
  }
  auto iit = in_edges_.find(id);
  if (iit != in_edges_.end()) {
    for (auto eid : iit->second) {
      if (std::find(set_members.begin(), set_members.end(), eid) != set_members.end()) continue;
      const Edge* e = find_edge_locked(eid);
      if (!e || !e->active) continue;
      bool b = false, d = false, st = false;
      std::vector<BlockingDependency> tmp;
      eval_edge_locked(*e, tmp, b, d, st);
      if (b || st) for (auto& x : tmp) if (!x.optional) ex.blockers.push_back(std::move(x));
    }
  }

  if (n->ever_invalidated) {
    ex.upstream_invalidation_notes.push_back(
        "affected by invalidation reason: " + std::string(name_of(n->last_invalidation_reason)));
  }
  if (n->was_ready_before_restart && n->state != ReadinessState::READY) {
    ex.changes_since_ready.push_back(
        "node was READY before coordinator restart; dynamic evidence now requires revalidation");
  }
  if (!n->superseded_generations.empty()) {
    for (auto& g : n->superseded_generations) {
      ex.changes_since_ready.push_back("superseded generation " + g.to_string() +
                                       " is no longer authoritative");
    }
  }

  switch (n->state) {
    case ReadinessState::READY: ex.required_actions.push_back("none"); break;
    case ReadinessState::BLOCKED: ex.required_actions.push_back("satisfy blocking dependencies"); break;
    case ReadinessState::STALE: ex.required_actions.push_back("recompute/rebind against current authoritative generation"); break;
    case ReadinessState::REVALIDATION_REQUIRED: ex.required_actions.push_back("republish fresh dynamic evidence"); break;
    case ReadinessState::INVALIDATED: ex.required_actions.push_back("recover/recompute the invalidated node"); break;
    case ReadinessState::DEGRADED: ex.required_actions.push_back("restore optional dependency or tolerate degraded operation"); break;
    default: break;
  }
  return ex;
}

}  // namespace dependency_fabric