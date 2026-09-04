// dependency_fabric::Persistence — versioned binary persistence with integrity
// checking and conservative recovery.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The persisted format is versioned, self-describing, and integrity-checked.
// It is parsed with strict bounds checks and validated semantically before it
// is allowed to reconstruct a graph. Malformed, truncated, corrupt, or
// inconsistent payloads are rejected rather than silently accepted.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "graph.hpp"

namespace dependency_fabric {

struct PersistenceFormat {
  static constexpr std::uint32_t kMagic = 0x44504641u;   // "DPFA"
  static constexpr std::uint16_t kVersion = 1;
};

class Persistence {
 public:
  static std::optional<std::vector<std::uint8_t>> save(const Graph& graph, std::string* error);
  static Outcome load(const std::vector<std::uint8_t>& bytes, Graph* graph, std::string* error);

  static Outcome save_file(const Graph& graph, const std::string& path, std::string* error);
  static Outcome load_file(const std::string& path, Graph* graph, std::string* error);
};

}  // namespace dependency_fabric
