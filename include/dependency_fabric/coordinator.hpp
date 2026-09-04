// dependency_fabric::Coordinator — applies the framed protocol onto a Graph and
// serves a loopback TCP endpoint.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "graph.hpp"
#include "protocol.hpp"

namespace dependency_fabric {

// ---------------------------------------------------------------------------
// Message builders (used by coordinator clients such as workers/consumer).
// ---------------------------------------------------------------------------
proto::Frame hello_req();
proto::Frame register_req(SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
proto::Frame publish_node_req(DependencyNodeId node, NodeKind kind, std::string descriptor,
                              SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
proto::Frame publish_gen_req(DependencyNodeId node, DependencyNodeGeneration gen,
                             SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
proto::Frame add_edge_req(const Edge& edge, SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
proto::Frame query_req(DependencyNodeId node);
proto::Frame advance_epoch_req(CoordinatorEpoch epoch);
proto::Frame invalidate_req(DependencyNodeId node, InvalidationReason reason, InvalidationMode mode,
                            std::uint32_t depth, bool propagate_optional, bool degrade_optional,
                            SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
proto::Frame mark_recovered_req(DependencyNodeId node, SourceId source, SourceBootId boot,
                                CoordinatorEpoch epoch);
proto::Frame remove_edge_req(DependencyEdgeId id, SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
proto::Frame publish_readiness_req(DependencyNodeId node, SourceId source, SourceBootId boot, CoordinatorEpoch epoch);
proto::Frame persist_req(const std::string& path);
proto::Frame bye();

// Response parsers.
bool parse_hello_resp(const proto::Frame& frame, CoordinatorEpoch& epoch);
bool parse_outcome_resp(const proto::Frame& frame, Outcome& out);
bool parse_query_resp(const proto::Frame& frame, ReadinessState& state,
                      DependencyNodeGeneration& gen,
                      std::vector<BlockingDependency>& blockers);
bool parse_persist_resp(const proto::Frame& frame, Outcome& out, std::uint64_t& digest);

// Convenience: true iff a message represents success (ACCEPTED).
bool is_accepted(const proto::Frame& frame);

// ---------------------------------------------------------------------------
// A tiny synchronous client for one request/response exchange.
// ---------------------------------------------------------------------------
class CoordinatorClient {
 public:
  bool open(const std::string& host, std::uint16_t port, std::string* err);
  void close();
  std::optional<proto::Frame> exchange(const proto::Frame& req, std::string* err);
  proto::NetStream* stream() { return &stream_; }

 private:
  proto::NetStream stream_;
};

class Coordinator {
 public:
  explicit Coordinator(Graph* graph) : graph_(graph) {}

  // Apply a request frame and produce a response frame. Returns nullopt for
  // requests that legitimately have no response (e.g. BYE), or on a fatal
  // decode error (the caller should drop the connection).
  std::optional<proto::Frame> handle(const proto::Frame& req);

  // Serve loopback TCP on |port| until |stop| is set. Accepts connections one
  // at a time and processes each client's frames until that client closes.
  void serve(std::uint16_t port, const std::atomic<bool>* stop);

  Graph* graph() { return graph_; }

 private:
  Graph* graph_;
};

}  // namespace dependency_fabric