// dependency_fabric::dependency_set — first-class logical prerequisites.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <vector>
#include "identity.hpp"
#include "enums.hpp"

namespace dependency_fabric {

struct DependencySet {
  DependencySetId id;
  DependencySetGeneration generation;
  DependencyNodeId owner;    // the consumer node this set belongs to
  SetOperator op = SetOperator::ALL_OF;
  std::uint32_t at_least_n = 1;  // meaningful for AT_LEAST_N / ANY_OF
  std::vector<DependencyEdgeId> members;  // member edges (all inbound to owner)
  bool active = true;
};

}  // namespace dependency_fabric
