// dependency_fabric::adapters — reference adapter implementations.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dependency_fabric/adapters.hpp"

namespace dependency_fabric {

std::string_view NullAdapter::owning_runtime(NodeKind kind) const {
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
    case NodeKind::UNKNOWN: break;
  }
  return "DependencyFabric";
}

std::string_view ReferenceAdapter::owning_runtime(NodeKind kind) const {
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
    case NodeKind::UNKNOWN: break;
  }
  return "DependencyFabric";
}

}  // namespace dependency_fabric
