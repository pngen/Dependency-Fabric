// dependency_fabric::predicates — declarative dependency predicates.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Predicates are *data*, not code. They state the declarative constraints a
// producer must satisfy for a consumer to become READY. Evaluation is
// deterministic and inspectable. No user-supplied callable is ever executed
// as part of predicate evaluation.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "identity.hpp"
#include "enums.hpp"

namespace dependency_fabric {

struct DependencyPredicate {
  // Generation constraint on the producer.
  GenerationPredicateKind generation_kind = GenerationPredicateKind::NONE;

  // Valid only when generation_kind == EXACT.
  DependencyNodeGeneration exact_generation;
  // Valid only when generation_kind == MINIMUM.
  DependencyNodeGeneration minimum_generation;
  // Valid only when generation_kind == COMPATIBLE_SET.
  std::vector<DependencyNodeGeneration> compatible_generations;

  // Required producer readiness level (REQUIRES_READY).
  ReadinessState required_readiness = ReadinessState::READY;

  // REQUIRES_CAPABILITY: named capability that must be claimed by the producer.
  std::string required_capability;

  // REQUIRES_RESOURCE: producer must be an available resource.
  bool requires_resource_available = false;

  // REQUIRES_FRESH: producer must still be fresh (within its validity window).
  bool requires_fresh = false;

  // REQUIRES_INTEGRITY: producer must pass integrity validation.
  bool requires_integrity = false;

  // Minimal provenance confidence (0..100) required for the producer.
  std::uint64_t minimal_provenance_confidence = 0;

  // Optional completion-state requirement (e.g. for EXECUTION_RESULT producers).
  std::string required_completion_state;

  // Whether this edge hard-blocks the consumer when unsatisfied. Optional
  // edges that fail downgrade to DEGRADED rather than BLOCKED.
  bool is_required = true;

  // Human-readable explanation of the constraint (used in blockers/why-queries).
  std::string note;
};

}  // namespace dependency_fabric
