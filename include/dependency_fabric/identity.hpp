// dependency_fabric::identity — strongly typed identities and generations.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every identity and every generation in Dependency Fabric is a distinct
// C++ type. Values that carry the same numeric value but belong to different
// authority domains are *not* interchangeable: converting a ResourceGeneration
// into an ArtifactGeneration is a type error, and converting a SourceBootId
// into a SourceId is a type error. Only identities/generations that
// semantically denote the same thing are allowed to interoperate (for
// example, ProducerId and DependencyNodeId both name a node).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <compare>
#include <string>

namespace dependency_fabric {

// ---------------------------------------------------------------------------
// Tag family trait.
//
// Each tag declares which "family" of semantics it belongs to. Conversions
// (and only conversions) are permitted between strong numbers whose tags
// share a family. Families represent genuinely different authority domains,
// so a value never silently crosses domains.
// ---------------------------------------------------------------------------
template <typename T>
struct TagFamily : std::integral_constant<std::size_t, 0> {};

// Identity tag structs.
struct DependencyNodeIdTag {};
struct ProducerIdTag {};
struct ConsumerIdTag {};
struct DependencyEdgeIdTag {};
struct DependencySetIdTag {};
struct CoordinatorEpochTag {};
struct SourceIdTag {};
struct SourceBootIdTag {};

// Generation tag structs.
struct DependencyNodeGenerationTag {};
struct ProducerGenerationTag {};
struct ConsumerGenerationTag {};
struct DependencyEdgeGenerationTag {};
struct DependencySetGenerationTag {};
struct ArtifactGenerationTag {};
struct StateGenerationTag {};
struct ResourceGenerationTag {};
struct ReadinessGenerationTag {};
struct InvalidationGenerationTag {};
struct RecoveryGenerationTag {};
struct PolicyGenerationTag {};

// Family ids.
constexpr std::size_t kFamilyNodeId = 1;
constexpr std::size_t kFamilyNodeGeneration = 2;
constexpr std::size_t kFamilyEdgeId = 3;
constexpr std::size_t kFamilyEdgeGeneration = 4;
constexpr std::size_t kFamilySetId = 5;
constexpr std::size_t kFamilySetGeneration = 6;
constexpr std::size_t kFamilyCoordinatorEpoch = 7;
constexpr std::size_t kFamilySourceId = 8;
constexpr std::size_t kFamilySourceBootId = 9;
constexpr std::size_t kFamilyArtifactGeneration = 10;
constexpr std::size_t kFamilyStateGeneration = 11;
constexpr std::size_t kFamilyResourceGeneration = 12;
constexpr std::size_t kFamilyReadinessGeneration = 13;
constexpr std::size_t kFamilyInvalidationGeneration = 14;
constexpr std::size_t kFamilyRecoveryGeneration = 15;
constexpr std::size_t kFamilyPolicyGeneration = 16;

// Node identities interoperate.
template <> struct TagFamily<DependencyNodeIdTag> : std::integral_constant<std::size_t, kFamilyNodeId> {};
template <> struct TagFamily<ProducerIdTag>       : std::integral_constant<std::size_t, kFamilyNodeId> {};
template <> struct TagFamily<ConsumerIdTag>       : std::integral_constant<std::size_t, kFamilyNodeId> {};

// Node generations interoperate (edge bindings use producer/consumer mints).
template <> struct TagFamily<DependencyNodeGenerationTag> : std::integral_constant<std::size_t, kFamilyNodeGeneration> {};
template <> struct TagFamily<ProducerGenerationTag>       : std::integral_constant<std::size_t, kFamilyNodeGeneration> {};
template <> struct TagFamily<ConsumerGenerationTag>       : std::integral_constant<std::size_t, kFamilyNodeGeneration> {};

template <> struct TagFamily<DependencyEdgeIdTag>       : std::integral_constant<std::size_t, kFamilyEdgeId> {};
template <> struct TagFamily<DependencyEdgeGenerationTag> : std::integral_constant<std::size_t, kFamilyEdgeGeneration> {};
template <> struct TagFamily<DependencySetIdTag>        : std::integral_constant<std::size_t, kFamilySetId> {};
template <> struct TagFamily<DependencySetGenerationTag> : std::integral_constant<std::size_t, kFamilySetGeneration> {};

template <> struct TagFamily<CoordinatorEpochTag>   : std::integral_constant<std::size_t, kFamilyCoordinatorEpoch> {};
template <> struct TagFamily<SourceIdTag>           : std::integral_constant<std::size_t, kFamilySourceId> {};
template <> struct TagFamily<SourceBootIdTag>       : std::integral_constant<std::size_t, kFamilySourceBootId> {};

// Each generation axis is its own non-interoperable domain.
template <> struct TagFamily<ArtifactGenerationTag>    : std::integral_constant<std::size_t, kFamilyArtifactGeneration> {};
template <> struct TagFamily<StateGenerationTag>       : std::integral_constant<std::size_t, kFamilyStateGeneration> {};
template <> struct TagFamily<ResourceGenerationTag>    : std::integral_constant<std::size_t, kFamilyResourceGeneration> {};
template <> struct TagFamily<ReadinessGenerationTag>   : std::integral_constant<std::size_t, kFamilyReadinessGeneration> {};
template <> struct TagFamily<InvalidationGenerationTag>: std::integral_constant<std::size_t, kFamilyInvalidationGeneration> {};
template <> struct TagFamily<RecoveryGenerationTag>    : std::integral_constant<std::size_t, kFamilyRecoveryGeneration> {};
template <> struct TagFamily<PolicyGenerationTag>      : std::integral_constant<std::size_t, kFamilyPolicyGeneration> {};

// ---------------------------------------------------------------------------
// Strong — a distinct, value-typed identity/generation.
// ---------------------------------------------------------------------------
template <typename Tag>
class Strong {
 public:
  using tag_type = Tag;
  using value_type = std::uint64_t;

  // 0 is the "null"/invalid value across all domains.
  static constexpr value_type NullValue = 0;

  constexpr Strong() noexcept : value_(NullValue) {}
  explicit constexpr Strong(value_type v) noexcept : value_(v) {}
  constexpr Strong(const Strong&) noexcept = default;
  constexpr Strong& operator=(const Strong&) noexcept = default;

  // Convert only within the same semantic family.
  template <typename OtherTag>
  requires (TagFamily<Tag>::value == TagFamily<OtherTag>::value)
  constexpr Strong(Strong<OtherTag> other) noexcept : value_(other.value()) {}

  [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != NullValue; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

  friend constexpr bool operator==(const Strong&, const Strong&) noexcept = default;
  friend constexpr auto operator<=>(const Strong&, const Strong&) noexcept = default;

  [[nodiscard]] std::string to_string() const { return std::to_string(value_); }
 private:
  value_type value_;
};

// Convenience aliases used throughout the API.
using DependencyNodeId        = Strong<DependencyNodeIdTag>;
using DependencyNodeGeneration = Strong<DependencyNodeGenerationTag>;
using DependencyEdgeId        = Strong<DependencyEdgeIdTag>;
using DependencyEdgeGeneration = Strong<DependencyEdgeGenerationTag>;
using DependencySetId         = Strong<DependencySetIdTag>;
using DependencySetGeneration = Strong<DependencySetGenerationTag>;
using CoordinatorEpoch        = Strong<CoordinatorEpochTag>;
using SourceId                = Strong<SourceIdTag>;
using SourceBootId            = Strong<SourceBootIdTag>;

using ProducerId              = Strong<ProducerIdTag>;
using ProducerGeneration      = Strong<ProducerGenerationTag>;
using ConsumerId              = Strong<ConsumerIdTag>;
using ConsumerGeneration      = Strong<ConsumerGenerationTag>;

using ArtifactGeneration      = Strong<ArtifactGenerationTag>;
using StateGeneration         = Strong<StateGenerationTag>;
using ResourceGeneration      = Strong<ResourceGenerationTag>;
using ReadinessGeneration     = Strong<ReadinessGenerationTag>;
using InvalidationGeneration  = Strong<InvalidationGenerationTag>;
using RecoveryGeneration      = Strong<RecoveryGenerationTag>;
using PolicyGeneration        = Strong<PolicyGenerationTag>;

}  // namespace dependency_fabric

namespace std {
template <typename Tag>
struct hash<dependency_fabric::Strong<Tag>> {
  std::size_t operator()(const dependency_fabric::Strong<Tag>& s) const noexcept {
    return std::hash<std::uint64_t>()(s.value());
  }
};
}  // namespace std