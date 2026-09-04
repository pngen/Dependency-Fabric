// dependency_fabric::Graph — the explicit dependency graph and its current
// authority/readiness semantics.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The Graph owns:
//  * nodes, edges, and dependency sets;
//  * current authoritative generations;
//  * readiness state and its evaluation;
//  * invalidation, supersession, and stale-authority rejection;
//  * recovery-obligation determination (it never performs recovery itself);
//  * deterministic traversal/closure queries;
//  * append-only generation history with current-authority pointers.
//
// The Graph is thread-safe. Mutations take an exclusive lock; pure queries take
// a shared lock. Query-heavy workloads therefore scale to concurrent readers.

#pragma once

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <string>
#include <optional>
#include <cstdint>

#include "identity.hpp"
#include "enums.hpp"
#include "node.hpp"
#include "edge.hpp"
#include "dependency_set.hpp"
#include "predicates.hpp"
#include "queries.hpp"
#include "adapters.hpp"

namespace dependency_fabric {

// Default safety cap for recursive/bounded operations (depth + count) so that a
// hostile graph cannot cause unbounded traversal or stack exhaustion.
constexpr std::size_t kDefaultMaxTraversalDepth = 4096;
constexpr std::size_t kMaxGraphNodes = 1U << 24;   // 16M nodes hard cap
constexpr std::size_t kMaxGraphEdges = 1U << 26;   // 67M edges hard cap

// A patch of evidence facts applied to a node by set_node_evidence.
struct NodeEvidencePatch {
  std::vector<std::string> claimed_capabilities;
  bool set_resource_available = false; bool resource_available = false;
  bool set_integrity = false; bool integrity_valid = false;
  bool set_freshness = false; bool freshness_valid = false;
  bool set_provenance = false; std::uint64_t provenance_confidence = 0;
  bool set_completion = false; std::string completion_state;
};

class Graph {
 public:
  Graph() = default;
  ~Graph() = default;

  Graph(const Graph&) = delete;
  Graph& operator=(const Graph&) = delete;

  // Attach/detach an external adapter. nullptr restores self-contained mode.
  void set_adapter(const IDependencyAdapter* adapter) noexcept;

  // --- authority -----------------------------------------------------------
  CoordinatorEpoch current_epoch() const;
  Outcome register_source(SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
  Outcome advance_epoch(CoordinatorEpoch new_epoch);
  std::optional<SourceRegistration> source_registration(SourceId source) const;

  // --- node mutation -------------------------------------------------------
  Outcome declare_node(DependencyNodeId id, NodeKind kind, std::string descriptor,
                       SourceId source, SourceBootId boot, CoordinatorEpoch epoch,
                       bool static_durable_fact = false);
  Outcome publish_generation(DependencyNodeId id, DependencyNodeGeneration gen,
                             SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
  Outcome supersede_generation(DependencyNodeId id, DependencyNodeGeneration old_gen,
                               DependencyNodeGeneration new_gen,
                               SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
  Outcome set_node_evidence(DependencyNodeId id, const NodeEvidencePatch& patch,
                            SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
  Outcome retire_node(DependencyNodeId id,
                      SourceId source, SourceBootId boot, CoordinatorEpoch epoch);

  // --- edge mutation -------------------------------------------------------
  Outcome add_edge(const Edge& edge, SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
  Outcome remove_edge(DependencyEdgeId id,
                      SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
  Outcome set_edge_active(DependencyEdgeId id, bool active,
                          SourceId source, SourceBootId boot, CoordinatorEpoch epoch);

  // --- dependency set mutation ---------------------------------------------
  Outcome add_dependency_set(const DependencySet& set,
                             SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
  Outcome remove_dependency_set(DependencySetId id,
                                SourceId source, SourceBootId boot, CoordinatorEpoch epoch);

  // --- readiness -----------------------------------------------------------
  Outcome publish_readiness(DependencyNodeId id, ReadinessState state,
                            SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
  Outcome recompute_readiness(DependencyNodeId id);
  Outcome recompute_all_readiness();

  // --- invalidation / supersession / stale ---------------------------------
  Outcome invalidate_node(DependencyNodeId id, InvalidationReason reason,
                          const InvalidationPolicy& policy,
                          SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
  Outcome invalidate_generation(DependencyNodeId id, DependencyNodeGeneration gen,
                                InvalidationReason reason, const InvalidationPolicy& policy,
                                SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
  Outcome mark_stale(DependencyNodeId id,
                     SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
  Outcome mark_missing(DependencyNodeId id,
                       SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
  Outcome mark_recovered(DependencyNodeId id,
                         SourceId source, SourceBootId boot, CoordinatorEpoch epoch);

  // --- queries (shared lock, const) ---------------------------------------
  ReadinessState readiness(DependencyNodeId id) const;
  std::optional<Node> node(DependencyNodeId id) const;
  std::optional<Edge> edge(DependencyEdgeId id) const;
  std::optional<DependencySet> dependency_set(DependencySetId id) const;
  std::vector<DependencyNodeId> all_nodes() const;
  std::vector<DependencyEdgeId> all_edges() const;
  std::size_t node_count() const;
  std::size_t edge_count() const;
  std::size_t set_count() const;
  std::size_t source_count() const;

  Explanation explain(DependencyNodeId id) const;
  std::vector<BlockingDependency> blockers(DependencyNodeId id) const;
  std::vector<DependencyNodeId> dependents(DependencyNodeId id, bool transitive) const;
  std::vector<DependencyNodeId> prerequisites(DependencyNodeId id, bool transitive) const;
  AffectedClosure affected_closure(DependencyNodeId seed, std::uint32_t max_depth) const;
  std::vector<DependencyNodeId> recovery_set(DependencyNodeId seed) const;
  std::vector<DependencyNodeId> recompute_frontier(DependencyNodeId seed) const;
  RecoveryPlan recovery_plan(DependencyNodeId seed) const;
  DependencyNodeGeneration current_authority(DependencyNodeId id) const;

  // --- cycle / structure diagnostics --------------------------------------
  bool acyclic() const;   // verifies the whole graph is acyclic
  std::vector<DependencyEdgeId> self_loop_edges() const;
  std::vector<DependencyNodeId> duplicate_id_nodes() const;

  // --- persistence ---------------------------------------------------------
  std::optional<std::vector<std::uint8_t>> serialize() const;
  std::uint64_t digest() const;

 private:
  friend class Persistence;
  friend class PersistenceLoader;

  static ReadinessState state_after_recovery(ReadinessState recovered, bool durable);

  [[nodiscard]] bool authority_ok(SourceId source, SourceBootId boot, CoordinatorEpoch epoch,
                                  Outcome& reject) const;
  [[nodiscard]] Node* find_locked(DependencyNodeId id);
  [[nodiscard]] const Node* find_locked(DependencyNodeId id) const;
  [[nodiscard]] Edge* find_edge_locked(DependencyEdgeId id);
  [[nodiscard]] const Edge* find_edge_locked(DependencyEdgeId id) const;
  void rebuild_indexes_locked();
  [[nodiscard]] bool would_create_cycle_locked(DependencyNodeId producer,
                                               DependencyNodeId consumer) const;
  void propagate_invalidation_locked(DependencyNodeId seed, const InvalidationPolicy& policy,
                                     InvalidationReason reason, std::uint32_t depth_remaining);
  Outcome eval_edge_locked(const Edge& edge,
                                         std::vector<BlockingDependency>& blockers,
                                         bool& blocked, bool& degraded, bool& stale) const;
  Outcome eval_set_locked(const DependencySet& set,
                                        std::vector<BlockingDependency>& blockers,
                                        bool& blocked, bool& degraded, bool& stale,
                                        std::uint32_t& satisfied) const;
  Outcome recompute_node_locked(DependencyNodeId id);
  void recompute_all_locked();
  void recompute_closure_locked(std::vector<DependencyNodeId> roots);
  [[nodiscard]] NodeList descendants_locked(DependencyNodeId id, bool transitive,
                                            bool through_optional) const;
  [[nodiscard]] NodeList ancestors_locked(DependencyNodeId id, bool transitive,
                                          bool through_optional) const;
  [[nodiscard]] bool meets_readiness_constraint(ReadinessState actual,
                                                ReadinessState required) const;
  [[nodiscard]] bool meets_generation_constraint(const Node& producer,
                                                 const DependencyPredicate& pred,
                                                 DependencyNodeGeneration& required,
                                                 GenerationPredicateKind& kind) const;
  AffectedClosure affected_closure_locked(DependencyNodeId seed, std::uint32_t max_depth) const;
    [[nodiscard]] std::vector<DependencyNodeId> topology_order_locked(
      const std::vector<DependencyNodeId>& start) const;

  mutable std::shared_mutex mutex_;
  friend class GraphAccess;  // test hook

  std::unordered_map<DependencyNodeId, Node> nodes_;
  std::unordered_map<DependencyEdgeId, Edge> edges_;
  std::unordered_map<DependencySetId, DependencySet> sets_;
  std::unordered_map<SourceId, SourceRegistration> sources_;

  // Indexed adjacency: out_edges_[X] = edges where X is the producer (dependents
  // of X); in_edges_[X] = edges where X is the consumer (prerequisites of X).
  std::unordered_map<DependencyNodeId, std::vector<DependencyEdgeId>> out_edges_;
  std::unordered_map<DependencyNodeId, std::vector<DependencyEdgeId>> in_edges_;
  std::unordered_map<DependencyNodeId, std::vector<DependencySetId>> node_sets_;

  CoordinatorEpoch epoch_ = CoordinatorEpoch(1);  // genesis epoch
  const IDependencyAdapter* adapter_ = nullptr;
};

}  // namespace dependency_fabric