// dependency_fabric — public umbrella header.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Dependency Fabric is an open-source, vendor-neutral C++20 runtime for
// governing dependency identity, readiness, generations, invalidation,
// propagation, recovery, and execution eligibility across data, state, models,
// artifacts, resources, and distributed AI infrastructure.

#pragma once

#include "dependency_fabric/identity.hpp"
#include "dependency_fabric/enums.hpp"
#include "dependency_fabric/predicates.hpp"
#include "dependency_fabric/node.hpp"
#include "dependency_fabric/edge.hpp"
#include "dependency_fabric/dependency_set.hpp"
#include "dependency_fabric/queries.hpp"
#include "dependency_fabric/adapters.hpp"
#include "dependency_fabric/graph.hpp"
#include "dependency_fabric/persistence.hpp"
