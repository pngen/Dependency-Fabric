// dependency_fabric::graph — core graph implementation (authority, node/edge/
// set mutation, structure helpers).
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dependency_fabric/graph.hpp"

#include <algorithm>
#include <deque>
#include <unordered_set>

namespace dependency_fabric {

namespace {

}  // namespace

// ---------------------------------------------------------------------------
// Adapter
// ---------------------------------------------------------------------------
void Graph::set_adapter(const IDependencyAdapter* adapter) noexcept {
  std::unique_lock lock(mutex_);
  adapter_ = adapter;
  recompute_all_locked();
}

// ---------------------------------------------------------------------------
// Authority
// ---------------------------------------------------------------------------
CoordinatorEpoch Graph::current_epoch() const {
  std::shared_lock lock(mutex_);
  return epoch_;
}

Outcome Graph::register_source(SourceId source, SourceBootId boot, CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  if (!source.valid()) return Outcome::REJECT_STALE_BOOT;
  if (!boot.valid()) return Outcome::REJECT_STALE_BOOT;
  if (!epoch.valid() || epoch.value() != epoch_.value()) return Outcome::REJECT_STALE_EPOCH;
  auto it = sources_.find(source);
  if (it != sources_.end()) {
    // A fresh boot replaces the prior incarnation.
    it->second.boot = boot;
    it->second.epoch = epoch;
    it->second.active = true;
    return Outcome::ACCEPTED;
  }
  sources_[source] = SourceRegistration{source, boot, epoch, true};
  return Outcome::ACCEPTED;
}

Outcome Graph::advance_epoch(CoordinatorEpoch new_epoch) {
  std::unique_lock lock(mutex_);
  if (!new_epoch.valid() || new_epoch.value() <= epoch_.value()) {
    return Outcome::REJECT_STALE_EPOCH;
  }
  epoch_ = new_epoch;
  return Outcome::ACCEPTED;
}

std::optional<SourceRegistration> Graph::source_registration(SourceId source) const {
  std::shared_lock lock(mutex_);
  auto it = sources_.find(source);
  if (it == sources_.end()) return std::nullopt;
  return it->second;
}

// ---------------------------------------------------------------------------
// Lookup helpers
// ---------------------------------------------------------------------------
Node* Graph::find_locked(DependencyNodeId id) {
  auto it = nodes_.find(id);
  return it == nodes_.end() ? nullptr : &it->second;
}

const Node* Graph::find_locked(DependencyNodeId id) const {
  auto it = nodes_.find(id);
  return it == nodes_.end() ? nullptr : &it->second;
}

Edge* Graph::find_edge_locked(DependencyEdgeId id) {
  auto it = edges_.find(id);
  return it == edges_.end() ? nullptr : &it->second;
}

const Edge* Graph::find_edge_locked(DependencyEdgeId id) const {
  auto it = edges_.find(id);
  return it == edges_.end() ? nullptr : &it->second;
}

bool Graph::authority_ok(SourceId source, SourceBootId boot, CoordinatorEpoch epoch,
                         Outcome& reject) const {
  if (!epoch.valid() || epoch.value() != epoch_.value()) {
    reject = Outcome::REJECT_STALE_EPOCH;
    return false;
  }
  auto it = sources_.find(source);
  if (it == sources_.end() || !it->second.active) {
    reject = Outcome::REJECT_STALE_BOOT;
    return false;
  }
  if (boot.value() != it->second.boot.value()) {
    reject = Outcome::REJECT_STALE_BOOT;
    return false;
  }
  if (it->second.epoch.value() != epoch_.value()) {
    reject = Outcome::REJECT_STALE_EPOCH;
    return false;
  }
  return true;
}

void Graph::rebuild_indexes_locked() {
  out_edges_.clear();
  in_edges_.clear();
  node_sets_.clear();
  for (const auto& kv : edges_) {
    if (!kv.second.active) continue;
    out_edges_[kv.second.producer_id].push_back(kv.first);
    in_edges_[kv.second.consumer_id].push_back(kv.first);
  }
  for (const auto& kv : sets_) {
    if (!kv.second.active) continue;
    node_sets_[kv.second.owner].push_back(kv.first);
  }
}

bool Graph::would_create_cycle_locked(DependencyNodeId producer,
                                      DependencyNodeId consumer) const {
  if (producer.value() == consumer.value()) return true;  // self-cycle
  // Adding edge producer->consumer (consumer depends on producer). A cycle is
  // created iff producer is reachable from consumer by following edges
  // producer->consumer. BFS from consumer.
  std::unordered_set<std::uint64_t> visited;
  std::deque<DependencyNodeId> queue;
  queue.push_back(consumer);
  visited.insert(consumer.value());
  while (!queue.empty()) {
    auto cur = queue.front();
    queue.pop_front();
    auto it = out_edges_.find(cur);
    if (it == out_edges_.end()) continue;
    for (auto eid : it->second) {
      auto eit = edges_.find(eid);
      if (eit == edges_.end() || !eit->second.active) continue;
      auto next = eit->second.consumer_id;
      if (next.value() == producer.value()) return true;
      if (visited.insert(next.value()).second) queue.push_back(next);
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Node mutation
// ---------------------------------------------------------------------------
Outcome Graph::declare_node(DependencyNodeId id, NodeKind kind, std::string descriptor,
                            SourceId source, SourceBootId boot, CoordinatorEpoch epoch,
                            bool static_durable_fact) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  if (!id.valid()) return Outcome::REJECT_INVALID_TRANSITION;
  if (nodes_.count(id) != 0) return Outcome::REJECT_DUPLICATE_ID;
  if (nodes_.size() >= kMaxGraphNodes) return Outcome::REJECT_MALFORMED;
  Node n(id, kind, std::move(descriptor));
  n.static_durable_fact = static_durable_fact;
  n.dynamic_evidence = !static_durable_fact;
  n.origin_source = source;
  n.origin_boot = boot;
  n.state = ReadinessState::DECLARED;
  nodes_[id] = std::move(n);
  return Outcome::ACCEPTED;
}

Outcome Graph::publish_generation(DependencyNodeId id, DependencyNodeGeneration gen,
                                  SourceId source, SourceBootId boot, CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  Node* n = find_locked(id);
  if (!n) return Outcome::REJECT_UNKNOWN_DEPENDENCY;
  if (!gen.valid()) return Outcome::REJECT_INVALID_TRANSITION;
  // Generations never move backward.
  if (!n->current_generation.valid()) {
    // first published generation
  } else if (gen.value() <= n->current_generation.value()) {
    if (gen.value() == n->current_generation.value()) return Outcome::REJECT_DUPLICATE_ID;
    return Outcome::REJECT_STALE_NODE_GENERATION;
  }
  if (n->generation_history.size() >= kMaxGraphNodes) return Outcome::REJECT_MALFORMED;

  // Supersede the prior authoritative generation.
  if (n->current_generation.valid()) {
    n->superseded_generations.push_back(n->current_generation);
  }
  n->current_generation = gen;
  n->generation_history.push_back(gen);
  n->origin_source = source;
  n->origin_boot = boot;
  // A new generation requires revalidation; clear prior invalidation so that
  // recompute can re-derive state.
  n->ever_invalidated = false;
  n->state = ReadinessState::PRESENT;  // evidence present, recompute will finalize
  n->recovered_evidence = DynamicEvidence::FRESH;

  // Recompute this node and all its dependents, in upstream-first order.
  std::vector<DependencyNodeId> roots{id};
  recompute_closure_locked(roots);
  return Outcome::ACCEPTED;
}

Outcome Graph::supersede_generation(DependencyNodeId id, DependencyNodeGeneration old_gen,
                                    DependencyNodeGeneration new_gen,
                                    SourceId source, SourceBootId boot, CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  Node* n = find_locked(id);
  if (!n) return Outcome::REJECT_UNKNOWN_DEPENDENCY;
  if (!old_gen.valid() || !new_gen.valid()) return Outcome::REJECT_INVALID_TRANSITION;
  if (new_gen.value() <= old_gen.value()) return Outcome::REJECT_STALE_NODE_GENERATION;
  if (!n->has_generation(old_gen)) return Outcome::REJECT_STALE_NODE_GENERATION;
  if (n->highest_generation().value() == old_gen.value() &&
      n->current_generation.value() == old_gen.value()) {
    // Superseding the current authoritative generation moves it forward.
    if (new_gen.value() <= n->current_generation.value()) return Outcome::REJECT_STALE_NODE_GENERATION;
    n->superseded_generations.push_back(n->current_generation);
    n->current_generation = new_gen;
    n->generation_history.push_back(new_gen);
  } else {
    // Superseding a historical generation: record it.
    n->superseded_generations.push_back(old_gen);
    n->current_generation = new_gen;
    n->generation_history.push_back(new_gen);
  }
  n->origin_source = source;
  n->origin_boot = boot;
  n->ever_invalidated = false;
  n->state = ReadinessState::PRESENT;
  n->recovered_evidence = DynamicEvidence::FRESH;
  std::vector<DependencyNodeId> roots{id};
  recompute_closure_locked(roots);
  return Outcome::ACCEPTED;
}

Outcome Graph::set_node_evidence(DependencyNodeId id, const NodeEvidencePatch& patch,
                                 SourceId source, SourceBootId boot, CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  Node* n = find_locked(id);
  if (!n) return Outcome::REJECT_UNKNOWN_DEPENDENCY;
  if (patch.set_resource_available) n->resource_available = patch.resource_available;
  if (patch.set_integrity) n->integrity_valid = patch.integrity_valid;
  if (patch.set_freshness) n->freshness_valid = patch.freshness_valid;
  if (patch.set_provenance) n->provenance_confidence = patch.provenance_confidence;
  if (patch.set_completion) n->completion_state = patch.completion_state;
  if (!patch.claimed_capabilities.empty()) n->claimed_capabilities = patch.claimed_capabilities;
  std::vector<DependencyNodeId> roots{id};
  recompute_closure_locked(roots);
  return Outcome::ACCEPTED;
}

Outcome Graph::retire_node(DependencyNodeId id,
                           SourceId source, SourceBootId boot, CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  Node* n = find_locked(id);
  if (!n) return Outcome::REJECT_UNKNOWN_DEPENDENCY;
  n->state = ReadinessState::RETIRED;
  std::vector<DependencyNodeId> roots{id};
  recompute_closure_locked(roots);
  return Outcome::ACCEPTED;
}

// ---------------------------------------------------------------------------
// Edge mutation
// ---------------------------------------------------------------------------
Outcome Graph::add_edge(const Edge& edge, SourceId source, SourceBootId boot,
                        CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  if (!edge.id.valid()) return Outcome::REJECT_INVALID_TRANSITION;
  if (!edge.producer_id.valid() || !edge.consumer_id.valid()) return Outcome::REJECT_INVALID_TRANSITION;
  if (nodes_.count(edge.producer_id) == 0 || nodes_.count(edge.consumer_id) == 0) {
    return Outcome::REJECT_UNKNOWN_DEPENDENCY;
  }
  if (edges_.count(edge.id) != 0) return Outcome::REJECT_DUPLICATE_ID;
  if (edge.producer_id.value() == edge.consumer_id.value()) return Outcome::REJECT_CYCLE;
  // Duplicate equivalent edge (same producer, consumer, kind, active).
  auto po = out_edges_.find(edge.producer_id);
  if (po != out_edges_.end()) {
    for (auto eid : po->second) {
      const Edge& ex = edges_.at(eid);
      if (ex.consumer_id.value() == edge.consumer_id.value() && ex.kind == edge.kind && ex.active) {
        return Outcome::REJECT_DUPLICATE_ID;
      }
    }
  }
  if (would_create_cycle_locked(edge.producer_id, edge.consumer_id)) {
    return Outcome::REJECT_CYCLE;
  }
  if (edges_.size() >= kMaxGraphEdges) return Outcome::REJECT_MALFORMED;
  edges_[edge.id] = edge;
  out_edges_[edge.producer_id].push_back(edge.id);
  in_edges_[edge.consumer_id].push_back(edge.id);
  std::vector<DependencyNodeId> roots{edge.consumer_id};
  recompute_closure_locked(roots);
  return Outcome::ACCEPTED;
}

Outcome Graph::remove_edge(DependencyEdgeId id, SourceId source, SourceBootId boot,
                           CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  auto it = edges_.find(id);
  if (it == edges_.end()) return Outcome::REJECT_UNKNOWN_DEPENDENCY;
  DependencyNodeId consumer = it->second.consumer_id;
  DependencyNodeId producer = it->second.producer_id;
  const DependencyEdgeId eid = id;
  edges_.erase(it);
  auto erase_from = [](std::vector<DependencyEdgeId>& vec, DependencyEdgeId x) {
    vec.erase(std::remove(vec.begin(), vec.end(), x), vec.end());
  };
  erase_from(out_edges_[producer], eid);
  erase_from(in_edges_[consumer], eid);
  std::vector<DependencyNodeId> roots{consumer};
  recompute_closure_locked(roots);
  return Outcome::ACCEPTED;
}

Outcome Graph::set_edge_active(DependencyEdgeId id, bool active, SourceId source,
                               SourceBootId boot, CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  Edge* e = find_edge_locked(id);
  if (!e) return Outcome::REJECT_UNKNOWN_DEPENDENCY;
  if (e->active == active) { std::vector<DependencyNodeId> roots{e->consumer_id}; recompute_closure_locked(roots); return Outcome::ACCEPTED; }
  e->active = active;
  if (active) {
    out_edges_[e->producer_id].push_back(id);
    in_edges_[e->consumer_id].push_back(id);
  } else {
    auto erase_from = [](std::vector<DependencyEdgeId>& vec, DependencyEdgeId x) {
      vec.erase(std::remove(vec.begin(), vec.end(), x), vec.end());
    };
    erase_from(out_edges_[e->producer_id], id);
    erase_from(in_edges_[e->consumer_id], id);
  }
  std::vector<DependencyNodeId> roots{e->consumer_id};
  recompute_closure_locked(roots);
  return Outcome::ACCEPTED;
}

// ---------------------------------------------------------------------------
// Dependency set mutation
// ---------------------------------------------------------------------------
Outcome Graph::add_dependency_set(const DependencySet& set, SourceId source,
                                  SourceBootId boot, CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  if (!set.id.valid()) return Outcome::REJECT_INVALID_TRANSITION;
  if (nodes_.count(set.owner) == 0) return Outcome::REJECT_UNKNOWN_DEPENDENCY;
  if (sets_.count(set.id) != 0) return Outcome::REJECT_DUPLICATE_ID;
  // All member edges must exist and belong to the owner.
  for (auto eid : set.members) {
    auto eit = edges_.find(eid);
    if (eit == edges_.end()) return Outcome::REJECT_UNKNOWN_DEPENDENCY;
    if (eit->second.consumer_id.value() != set.owner.value()) return Outcome::REJECT_INVALID_TRANSITION;
  }
  sets_[set.id] = set;
  node_sets_[set.owner].push_back(set.id);
  std::vector<DependencyNodeId> roots{set.owner};
  recompute_closure_locked(roots);
  return Outcome::ACCEPTED;
}

Outcome Graph::remove_dependency_set(DependencySetId id, SourceId source, SourceBootId boot,
                                     CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  auto it = sets_.find(id);
  if (it == sets_.end()) return Outcome::REJECT_UNKNOWN_DEPENDENCY;
  DependencyNodeId owner = it->second.owner;
  sets_.erase(it);
  auto nsit = node_sets_.find(owner);
  if (nsit != node_sets_.end()) {
    auto& v = nsit->second;
    v.erase(std::remove(v.begin(), v.end(), id), v.end());
    if (v.empty()) node_sets_.erase(nsit);
  }
  std::vector<DependencyNodeId> roots{owner};
  recompute_closure_locked(roots);
  return Outcome::ACCEPTED;
}

// ---------------------------------------------------------------------------
// Readiness publish
// ---------------------------------------------------------------------------
Outcome Graph::publish_readiness(DependencyNodeId id, ReadinessState state, SourceId source,
                                 SourceBootId boot, CoordinatorEpoch epoch) {
  std::unique_lock lock(mutex_);
  Outcome rej = Outcome::UNKNOWN;
  if (!authority_ok(source, boot, epoch, rej)) return rej;
  Node* n = find_locked(id);
  if (!n) return Outcome::REJECT_UNKNOWN_DEPENDENCY;
  if (state == ReadinessState::READY) {
    // Fresh evidence: clear invalidation and revalidation gate.
    n->ever_invalidated = false;
    n->recovered_evidence = DynamicEvidence::FRESH;
    n->dynamic_evidence = true;
  } else {
    n->state = state;
  }
  std::vector<DependencyNodeId> roots{id};
  recompute_closure_locked(roots);
  return Outcome::ACCEPTED;
}

// ---------------------------------------------------------------------------
// Const queries
// ---------------------------------------------------------------------------
ReadinessState Graph::readiness(DependencyNodeId id) const {
  std::shared_lock lock(mutex_);
  const Node* n = find_locked(id);
  return n ? n->state : ReadinessState::UNKNOWN;
}

std::optional<Node> Graph::node(DependencyNodeId id) const {
  std::shared_lock lock(mutex_);
  const Node* n = find_locked(id);
  if (!n) return std::nullopt;
  return *n;
}

std::optional<Edge> Graph::edge(DependencyEdgeId id) const {
  std::shared_lock lock(mutex_);
  const Edge* e = find_edge_locked(id);
  if (!e) return std::nullopt;
  return *e;
}

std::optional<DependencySet> Graph::dependency_set(DependencySetId id) const {
  std::shared_lock lock(mutex_);
  auto it = sets_.find(id);
  if (it == sets_.end()) return std::nullopt;
  return it->second;
}

std::vector<DependencyNodeId> Graph::all_nodes() const {
  std::shared_lock lock(mutex_);
  std::vector<DependencyNodeId> out;
  out.reserve(nodes_.size());
  for (const auto& kv : nodes_) out.push_back(kv.first);
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<DependencyEdgeId> Graph::all_edges() const {
  std::shared_lock lock(mutex_);
  std::vector<DependencyEdgeId> out;
  out.reserve(edges_.size());
  for (const auto& kv : edges_) out.push_back(kv.first);
  std::sort(out.begin(), out.end());
  return out;
}

std::size_t Graph::node_count() const {
  std::shared_lock lock(mutex_);
  return nodes_.size();
}

std::size_t Graph::edge_count() const {
  std::shared_lock lock(mutex_);
  return edges_.size();
}

std::size_t Graph::set_count() const {
  std::shared_lock lock(mutex_);
  return sets_.size();
}

std::size_t Graph::source_count() const {
  std::shared_lock lock(mutex_);
  return sources_.size();
}

DependencyNodeGeneration Graph::current_authority(DependencyNodeId id) const {
  std::shared_lock lock(mutex_);
  const Node* n = find_locked(id);
  return n ? n->current_generation : DependencyNodeGeneration();
}

}  // namespace dependency_fabric