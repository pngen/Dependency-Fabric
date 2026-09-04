// dependency_fabric::adapters — narrow integration interfaces and reference
// adapters for standalone testing.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include "identity.hpp"
#include "enums.hpp"

namespace dependency_fabric {

class IDependencyAdapter {
 public:
  virtual ~IDependencyAdapter() = default;
  virtual std::optional<ArtifactGeneration> current_artifact_generation(DependencyNodeId node) const = 0;
  virtual std::optional<StateGeneration> current_state_generation(DependencyNodeId node) const = 0;
  virtual std::optional<ResourceGeneration> current_resource_generation(DependencyNodeId node) const = 0;
  virtual bool resource_available(DependencyNodeId node) const = 0;
  virtual bool capability_claimed(DependencyNodeId node, std::string_view capability) const = 0;
  virtual bool integrity_ok(DependencyNodeId node) const = 0;
  virtual bool freshness_ok(DependencyNodeId node) const = 0;
  virtual std::uint64_t provenance_confidence(DependencyNodeId node) const = 0;
  virtual std::string_view completion_state(DependencyNodeId node) const = 0;
  virtual std::string_view owning_runtime(NodeKind kind) const = 0;
};

// The default adapter used when no external runtime is attached. All lookups
// return conservative "unknown/false" so the graph falls back to the node's
// own recorded evidence facts.
class NullAdapter : public IDependencyAdapter {
 public:
  std::optional<ArtifactGeneration> current_artifact_generation(DependencyNodeId) const override { return std::nullopt; }
  std::optional<StateGeneration> current_state_generation(DependencyNodeId) const override { return std::nullopt; }
  std::optional<ResourceGeneration> current_resource_generation(DependencyNodeId) const override { return std::nullopt; }
  bool resource_available(DependencyNodeId) const override { return false; }
  bool capability_claimed(DependencyNodeId, std::string_view) const override { return false; }
  bool integrity_ok(DependencyNodeId) const override { return false; }
  bool freshness_ok(DependencyNodeId) const override { return false; }
  std::uint64_t provenance_confidence(DependencyNodeId) const override { return 0; }
  std::string_view completion_state(DependencyNodeId) const override { return {}; }
  std::string_view owning_runtime(NodeKind kind) const override;
};

// A simple in-memory fact store used as a standalone reference adapter for
// tests and examples. It mirrors an external artifact/state/resource author.
class ReferenceAdapter : public IDependencyAdapter {
 public:
  void set_artifact_generation(DependencyNodeId id, ArtifactGeneration g) { artifact_[id] = g; }
  void set_state_generation(DependencyNodeId id, StateGeneration g) { state_[id] = g; }
  void set_resource_generation(DependencyNodeId id, ResourceGeneration g) { resource_[id] = g; }
  void set_resource_available(DependencyNodeId id, bool v) { resource_avail_[id] = v; }
  void claim_capability(DependencyNodeId id, std::string cap) { capabilities_[id][std::move(cap)] = true; }
  void set_integrity(DependencyNodeId id, bool v) { integrity_[id] = v; }
  void set_freshness(DependencyNodeId id, bool v) { freshness_[id] = v; }
  void set_provenance(DependencyNodeId id, std::uint64_t v) { provenance_[id] = v; }
  void set_completion(DependencyNodeId id, std::string v) { completion_[id] = std::move(v); }

  std::optional<ArtifactGeneration> current_artifact_generation(DependencyNodeId id) const override {
    auto it = artifact_.find(id); return it == artifact_.end() ? std::nullopt : std::optional<ArtifactGeneration>(it->second);
  }
  std::optional<StateGeneration> current_state_generation(DependencyNodeId id) const override {
    auto it = state_.find(id); return it == state_.end() ? std::nullopt : std::optional<StateGeneration>(it->second);
  }
  std::optional<ResourceGeneration> current_resource_generation(DependencyNodeId id) const override {
    auto it = resource_.find(id); return it == resource_.end() ? std::nullopt : std::optional<ResourceGeneration>(it->second);
  }
  bool resource_available(DependencyNodeId id) const override { return get(resource_avail_, id); }
  bool capability_claimed(DependencyNodeId id, std::string_view cap) const override {
    auto it = capabilities_.find(id);
    return it != capabilities_.end() && it->second.count(std::string(cap)) != 0;
  }
  bool integrity_ok(DependencyNodeId id) const override { return get(integrity_, id); }
  bool freshness_ok(DependencyNodeId id) const override { return get(freshness_, id); }
  std::uint64_t provenance_confidence(DependencyNodeId id) const override {
    auto it = provenance_.find(id); return it == provenance_.end() ? 0 : it->second;
  }
  std::string_view completion_state(DependencyNodeId id) const override {
    auto it = completion_.find(id); return it == completion_.end() ? std::string_view{} : std::string_view(it->second);
  }
  std::string_view owning_runtime(NodeKind kind) const override;

 private:
  static bool get(const std::unordered_map<DependencyNodeId, bool>& m, DependencyNodeId id) {
    auto it = m.find(id); return it != m.end() && it->second;
  }
  std::unordered_map<DependencyNodeId, ArtifactGeneration> artifact_;
  std::unordered_map<DependencyNodeId, StateGeneration> state_;
  std::unordered_map<DependencyNodeId, ResourceGeneration> resource_;
  std::unordered_map<DependencyNodeId, bool> resource_avail_;
  std::unordered_map<DependencyNodeId, std::unordered_map<std::string, bool>> capabilities_;
  std::unordered_map<DependencyNodeId, bool> integrity_;
  std::unordered_map<DependencyNodeId, bool> freshness_;
  std::unordered_map<DependencyNodeId, std::uint64_t> provenance_;
  std::unordered_map<DependencyNodeId, std::string> completion_;
};

}  // namespace dependency_fabric