// dependency_fabric::graph — traversal, affected closure, and recovery planning.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dependency_fabric/graph.hpp"

#include <algorithm>
#include <deque>
#include <unordered_set>

namespace dependency_fabric {

namespace {
bool needs_recovery(ReadinessState s) {
  switch (s) {
    case ReadinessState::INVALIDATED:
    case ReadinessState::BLOCKED:
    case ReadinessState::STALE:
    case ReadinessState::REVALIDATION_REQUIRED:
    case ReadinessState::MISSING:
    case ReadinessState::FAILED:
    case ReadinessState::DEGRADED:
    case ReadinessState::SUPERSEDED:
      return true;
    default:
      return false;
  }
}

RecoveryAction action_for_state(ReadinessState s, NodeKind kind) {
  switch (s) {
    case ReadinessState::STALE:
    case ReadinessState::SUPERSEDED:
      return kind == NodeKind::SERVICE || kind == NodeKind::WORKLOAD_PHASE
                 ? RecoveryAction::RESTART
                 : RecoveryAction::RECOMPUTATION;
    case ReadinessState::INVALIDATED:
      if (kind == NodeKind::RESOURCE) return RecoveryAction::RESOURCE_REACQUISITION;
      if (kind == NodeKind::SERVICE || kind == NodeKind::WORKLOAD_PHASE) return RecoveryAction::RESTART;
      if (kind == NodeKind::CHECKPOINT || kind == NodeKind::KV_STATE) return RecoveryAction::CHECKPOINT_RESTORE;
      return RecoveryAction::RECOMPUTATION;
    case ReadinessState::BLOCKED:
      return RecoveryAction::REVALIDATION;
    case ReadinessState::REVALIDATION_REQUIRED:
      return RecoveryAction::REVALIDATION;
    case ReadinessState::MISSING:
      if (kind == NodeKind::RESOURCE) return RecoveryAction::RESOURCE_REACQUISITION;
      if (kind == NodeKind::ARTIFACT || kind == NodeKind::CHECKPOINT || kind == NodeKind::DATA || kind == NodeKind::TENSOR) {
        return RecoveryAction::RELOAD;
      }
      return RecoveryAction::RECOMPUTATION;
    case ReadinessState::FAILED:
      return RecoveryAction::RESTART;
    case ReadinessState::DEGRADED:
      return RecoveryAction::REVALIDATION;
    default:
      return RecoveryAction::REVALIDATION;
  }
}

const char* owner_for_kind(NodeKind kind) {
  switch (kind) {
    case NodeKind::ARTIFACT: return "ArtifactFabric";
    case NodeKind::MODEL: return "ModelRegistry";
    case NodeKind::ADAPTER: return "AdapterFabric";
    case NodeKind::KERNEL: return "KernelFabric";
    case NodeKind::EXECUTION_GRAPH: return "ExecutionGraphStore";
    case NodeKind::CHECKPOINT:
    case NodeKind::KV_STATE: return "CheckpointStore";
    case NodeKind::DATA:
    case NodeKind::TENSOR: return "DataStore";
    case NodeKind::RESOURCE: return "ResourceBroker";
    case NodeKind::SERVICE:
    case NodeKind::WORKLOAD_PHASE: return "WorkloadFabric";
    case NodeKind::CAPABILITY: return "CompatibilityRegistry";
    case NodeKind::EXECUTION_RESULT: return "ExecutionFabric";
    case NodeKind::POLICY: return "PolicyStore";
    case NodeKind::TOPOLOGY: return "TopologyRegistry";
    case NodeKind::UNKNOWN: return "DependencyFabric";
  }
  return "DependencyFabric";
}
}  // namespace

NodeList Graph::descendants_locked(DependencyNodeId id, bool transitive, bool through_optional) const {
  std::unordered_set<std::uint64_t> visited;
  std::deque<DependencyNodeId> q;
  NodeList result;
  q.push_back(id);
  visited.insert(id.value());
  while (!q.empty()) {
    auto cur = q.front();
    q.pop_front();
    auto it = out_edges_.find(cur);
    if (it == out_edges_.end()) continue;
    for (auto eid : it->second) {
      auto eit = edges_.find(eid);
      if (eit == edges_.end() || !eit->second.active) continue;
      const Edge& e = eit->second;
      if (e.kind == EdgeKind::OPTIONAL && !through_optional) continue;
      if (!transitive && cur.value() == id.value()) {
        // only direct
        auto nxt = e.consumer_id;
        if (visited.insert(nxt.value()).second) result.push_back(nxt);
        continue;
      }
      auto nxt = e.consumer_id;
      if (visited.insert(nxt.value()).second) {
        result.push_back(nxt);
        q.push_back(nxt);
      }
    }
  }
  return result;
}

NodeList Graph::ancestors_locked(DependencyNodeId id, bool transitive, bool through_optional) const {
  std::unordered_set<std::uint64_t> visited;
  std::deque<DependencyNodeId> q;
  NodeList result;
  q.push_back(id);
  visited.insert(id.value());
  while (!q.empty()) {
    auto cur = q.front();
    q.pop_front();
    auto it = in_edges_.find(cur);
    if (it == in_edges_.end()) continue;
    for (auto eid : it->second) {
      auto eit = edges_.find(eid);
      if (eit == edges_.end() || !eit->second.active) continue;
      const Edge& e = eit->second;
      if (e.kind == EdgeKind::OPTIONAL && !through_optional) continue;
      if (!transitive && cur.value() == id.value()) {
        auto nxt = e.producer_id;
        if (visited.insert(nxt.value()).second) result.push_back(nxt);
        continue;
      }
      auto nxt = e.producer_id;
      if (visited.insert(nxt.value()).second) {
        result.push_back(nxt);
        q.push_back(nxt);
      }
    }
  }
  return result;
}

std::vector<DependencyNodeId> Graph::dependents(DependencyNodeId id, bool transitive) const {
  std::shared_lock lock(mutex_);
  return descendants_locked(id, transitive, true);
}

std::vector<DependencyNodeId> Graph::prerequisites(DependencyNodeId id, bool transitive) const {
  std::shared_lock lock(mutex_);
  return ancestors_locked(id, transitive, true);
}

AffectedClosure Graph::affected_closure_locked(DependencyNodeId seed, std::uint32_t max_depth) const {
  AffectedClosure closure;
  if (!find_locked(seed)) return closure;
  const std::uint32_t cap = max_depth != 0 ? max_depth : kDefaultMaxTraversalDepth;
  std::unordered_set<std::uint64_t> visited;
  std::unordered_set<std::uint64_t> visited_edges;
  std::deque<std::pair<DependencyNodeId, std::uint32_t>> q;
  q.push_back({seed, 0});
  visited.insert(seed.value());
  closure.nodes.push_back(seed);
  while (!q.empty()) {
    auto [cur, depth] = q.front();
    q.pop_front();
    if (depth >= cap) continue;
    auto it = out_edges_.find(cur);
    if (it == out_edges_.end()) continue;
    for (auto eid : it->second) {
      auto eit = edges_.find(eid);
      if (eit == edges_.end() || !eit->second.active) continue;
      auto nxt = eit->second.consumer_id;
      if (visited_edges.insert(eid.value()).second) {
        closure.edges.push_back(eid);
      }
      if (visited.insert(nxt.value()).second) {
        closure.nodes.push_back(nxt);
        q.push_back({nxt, depth + 1});
      }
    }
  }
  std::unordered_set<std::uint64_t> node_set;
  for (auto n : closure.nodes) node_set.insert(n.value());
  for (auto n : closure.nodes) {
    bool has_child = false;
    auto it = out_edges_.find(n);
    if (it != out_edges_.end()) {
      for (auto eid : it->second) {
        auto eit = edges_.find(eid);
        if (eit == edges_.end() || !eit->second.active) continue;
        if (node_set.count(eit->second.consumer_id.value())) { has_child = true; break; }
      }
    }
    if (!has_child) closure.frontier.push_back(n);
  }
  return closure;
}

AffectedClosure Graph::affected_closure(DependencyNodeId seed, std::uint32_t max_depth) const {
  std::shared_lock lock(mutex_);
  return affected_closure_locked(seed, max_depth);
}

std::vector<DependencyNodeId> Graph::topology_order_locked(
    const std::vector<DependencyNodeId>& start) const {
  std::unordered_set<std::uint64_t> seen;
  std::deque<DependencyNodeId> q;
  std::vector<DependencyNodeId> order;
  for (auto s : start) if (s.valid() && seen.insert(s.value()).second) q.push_back(s);
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
  return order;
}

std::vector<DependencyNodeId> Graph::recovery_set(DependencyNodeId seed) const {
  std::shared_lock lock(mutex_);
  // Affected closure nodes that need recovery, ordered upstream-first.
  AffectedClosure c = affected_closure_locked(seed, 0);
  // affected_closure_locked assumes the shared lock we hold; re-derive order via topology.
  std::vector<DependencyNodeId> order = topology_order_locked(c.nodes);
  std::vector<DependencyNodeId> result;
  std::unordered_set<std::uint64_t> seen;
  for (auto n : order) {
    if (seen.count(n.value())) continue;
    seen.insert(n.value());
    const Node* nd = find_locked(n);
    if (nd && needs_recovery(nd->state)) result.push_back(n);
  }
  return result;
}

std::vector<DependencyNodeId> Graph::recompute_frontier(DependencyNodeId seed) const {
  std::shared_lock lock(mutex_);
  AffectedClosure c = affected_closure_locked(seed, 0);
  std::vector<DependencyNodeId> result;
  std::unordered_set<std::uint64_t> node_set;
  for (auto n : c.nodes) node_set.insert(n.value());
  // The recomputation frontier is the set of non-READY affected nodes whose
  // ancestors within the closure are all already READY (i.e. the boundary of
  // the region that must be recomputed, closest to the change).
  for (auto n : c.nodes) {
    const Node* nd = find_locked(n);
    if (!nd || nd->state == ReadinessState::READY) continue;
    bool has_nonready_ancestor = false;
    auto it = in_edges_.find(n);
    if (it != in_edges_.end()) {
      for (auto eid : it->second) {
        auto eit = edges_.find(eid);
        if (eit == edges_.end() || !eit->second.active) continue;
        auto anc = eit->second.producer_id;
        if (anc.value() != n.value() && node_set.count(anc.value())) {
          const Node* anc_node = find_locked(anc);
          if (anc_node && anc_node->state != ReadinessState::READY) { has_nonready_ancestor = true; break; }
        }
      }
    }
    if (!has_nonready_ancestor) result.push_back(n);
  }
  return result;
}

RecoveryPlan Graph::recovery_plan(DependencyNodeId seed) const {
  std::shared_lock lock(mutex_);
  RecoveryPlan plan;
  std::vector<DependencyNodeId> order = topology_order_locked({seed});
  std::unordered_set<std::uint64_t> seen;
  for (auto n : order) {
    if (seen.count(n.value())) continue;
    seen.insert(n.value());
    const Node* nd = find_locked(n);
    if (!nd || !needs_recovery(nd->state)) continue;
    RecoveryIntent intent;
    intent.node = n;
    intent.kind = nd->kind;
    intent.reason = nd->ever_invalidated ? nd->last_invalidation_reason : InvalidationReason::EXPLICIT;
    intent.action = action_for_state(nd->state, nd->kind);
    if (adapter_) {
      intent.owner_runtime = std::string(adapter_->owning_runtime(nd->kind));
    } else {
      intent.owner_runtime = owner_for_kind(nd->kind);
    }
    intent.detail = "node " + std::to_string(n.value()) + " (" + std::string(name_of(nd->kind)) +
                    ") is " + std::string(name_of(nd->state));
    plan.intents.push_back(std::move(intent));
  }
  return plan;
}

// ---------------------------------------------------------------------------
// Structure diagnostics.
// ---------------------------------------------------------------------------
bool Graph::acyclic() const {
  std::shared_lock lock(mutex_);
  std::unordered_map<DependencyNodeId, std::uint32_t> indeg;
  std::size_t edge_total = 0;
  for (const auto& kv : edges_) {
    (void)kv;
  }
  for (const auto& kv : nodes_) {
    indeg[kv.first] = 0;
  }
  for (const auto& kv : edges_) {
    if (!kv.second.active) continue;
    indeg[kv.second.consumer_id]++;
    ++edge_total;
  }
  std::deque<DependencyNodeId> q;
  for (const auto& kv : indeg) if (kv.second == 0) q.push_back(kv.first);
  std::size_t visited = 0;
  while (!q.empty()) {
    auto cur = q.front(); q.pop_front();
    ++visited;
    auto it = out_edges_.find(cur);
    if (it == out_edges_.end()) continue;
    for (auto eid : it->second) {
      auto eit = edges_.find(eid);
      if (eit == edges_.end() || !eit->second.active) continue;
      auto c = eit->second.consumer_id;
      if (--indeg[c] == 0) q.push_back(c);
    }
  }
  return visited == nodes_.size();
}

std::vector<DependencyEdgeId> Graph::self_loop_edges() const {
  std::shared_lock lock(mutex_);
  std::vector<DependencyEdgeId> result;
  for (const auto& kv : edges_) {
    if (kv.second.producer_id.value() == kv.second.consumer_id.value()) {
      result.push_back(kv.first);
    }
  }
  return result;
}

std::vector<DependencyNodeId> Graph::duplicate_id_nodes() const {
  // IDs are unique keys by construction; this reports nothing. Retained to give
  // an explicit invariant check surface.
  std::shared_lock lock(mutex_);
  std::vector<DependencyNodeId> result;
  std::unordered_set<std::uint64_t> seen;
  for (const auto& kv : nodes_) {
    if (!seen.insert(kv.first.value()).second) result.push_back(kv.first);
  }
  return result;
}

}  // namespace dependency_fabric