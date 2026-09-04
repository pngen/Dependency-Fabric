// dependency_fabric::Coordinator — protocol application + server loop.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dependency_fabric/coordinator.hpp"

#include "dependency_fabric/persistence.hpp"

#include <atomic>
#include <thread>

namespace dependency_fabric {

// ---- Message builders (requests) -----------------------------------------
proto::Frame hello_req() { return proto::Frame(proto::FrameType::HELLO_REQ, {}); }

proto::Frame register_req(SourceId s, SourceBootId b, CoordinatorEpoch e) {
  proto::Encoder enc; enc.u64(s.value()); enc.u64(b.value()); enc.u64(e.value());
  return proto::Frame(proto::FrameType::REGISTER_REQ, enc.take());
}

proto::Frame publish_node_req(DependencyNodeId n, NodeKind k, std::string desc, SourceId s,
                              SourceBootId b, CoordinatorEpoch e) {
  proto::Encoder enc; enc.u64(n.value()); enc.u8(static_cast<std::uint8_t>(k));
  enc.u64(s.value()); enc.u64(b.value()); enc.u64(e.value()); enc.str(desc);
  return proto::Frame(proto::FrameType::PUBLISH_NODE_REQ, enc.take());
}

proto::Frame publish_gen_req(DependencyNodeId n, DependencyNodeGeneration g, SourceId s,
                             SourceBootId b, CoordinatorEpoch e) {
  proto::Encoder enc; enc.u64(n.value()); enc.u64(g.value());
  enc.u64(s.value()); enc.u64(b.value()); enc.u64(e.value());
  return proto::Frame(proto::FrameType::PUBLISH_GEN_REQ, enc.take());
}

proto::Frame add_edge_req(const Edge& ed, SourceId s, SourceBootId b, CoordinatorEpoch e) {
  proto::Encoder enc;
  enc.u64(ed.id.value()); enc.u64(ed.generation.value());
  enc.u8(static_cast<std::uint8_t>(ed.kind));
  enc.u64(ed.producer_id.value()); enc.u64(ed.consumer_id.value());
  enc.u64(ed.bound_producer_generation.value()); enc.u64(ed.bound_consumer_generation.value());
  enc.u8(ed.active ? 1 : 0);
  const auto& p = ed.predicate;
  enc.u8(static_cast<std::uint8_t>(p.generation_kind));
  enc.u64(p.exact_generation.value()); enc.u64(p.minimum_generation.value());
  enc.u32(static_cast<std::uint32_t>(p.compatible_generations.size()));
  for (auto g : p.compatible_generations) enc.u64(g.value());
  enc.u8(static_cast<std::uint8_t>(p.required_readiness));
  enc.str(p.required_capability);
  enc.u8(p.requires_resource_available ? 1 : 0);
  enc.u8(p.requires_fresh ? 1 : 0);
  enc.u8(p.requires_integrity ? 1 : 0);
  enc.u64(p.minimal_provenance_confidence);
  enc.str(p.required_completion_state);
  enc.u8(p.is_required ? 1 : 0);
  enc.str(p.note);
  enc.u64(s.value()); enc.u64(b.value()); enc.u64(e.value());
  return proto::Frame(proto::FrameType::ADD_EDGE_REQ, enc.take());
}

proto::Frame query_req(DependencyNodeId n) {
  proto::Encoder enc; enc.u64(n.value()); return proto::Frame(proto::FrameType::QUERY_REQ, enc.take());
}

proto::Frame advance_epoch_req(CoordinatorEpoch e) {
  proto::Encoder enc; enc.u64(e.value()); return proto::Frame(proto::FrameType::INVALIDATE_REQ, enc.take());
}

proto::Frame invalidate_req(DependencyNodeId n, InvalidationReason r, InvalidationMode m,
                            std::uint32_t depth, bool prop, bool degr, SourceId s,
                            SourceBootId b, CoordinatorEpoch e) {
  proto::Encoder enc; enc.u64(n.value()); enc.u8(static_cast<std::uint8_t>(r));
  enc.u8(static_cast<std::uint8_t>(m)); enc.u32(depth);
  enc.u8(prop ? 1 : 0); enc.u8(degr ? 1 : 0);
  enc.u64(s.value()); enc.u64(b.value()); enc.u64(e.value());
  return proto::Frame(proto::FrameType::INVALIDATE_REQ, enc.take());
}

proto::Frame mark_recovered_req(DependencyNodeId n, SourceId s, SourceBootId b, CoordinatorEpoch e) {
  proto::Encoder enc; enc.u64(n.value()); enc.u64(s.value()); enc.u64(b.value()); enc.u64(e.value());
  return proto::Frame(proto::FrameType::MARK_RECOVERED_REQ, enc.take());
}

proto::Frame remove_edge_req(DependencyEdgeId id, SourceId s, SourceBootId b, CoordinatorEpoch e) {
  proto::Encoder enc; enc.u64(id.value()); enc.u64(s.value()); enc.u64(b.value()); enc.u64(e.value());
  return proto::Frame(proto::FrameType::REMOVE_EDGE_REQ, enc.take());
}

proto::Frame persist_req(const std::string& path) {
  proto::Encoder enc; enc.str(path); return proto::Frame(proto::FrameType::PERSIST_REQ, enc.take());
}

proto::Frame publish_readiness_req(DependencyNodeId n, SourceId s, SourceBootId b, CoordinatorEpoch e) {
  proto::Encoder enc; enc.u64(n.value()); enc.u64(s.value()); enc.u64(b.value()); enc.u64(e.value());
  return proto::Frame(proto::FrameType::PUBLISH_READINESS_REQ, enc.take());
}

proto::Frame bye() { return proto::Frame(proto::FrameType::BYE, {}); }

// ---- Response parsers -----------------------------------------------------
bool parse_hello_resp(const proto::Frame& f, CoordinatorEpoch& epoch) {
  if (f.type != proto::FrameType::HELLO_RESP) return false;
  proto::Decoder d(f.body.data(), f.body.size());
  epoch = CoordinatorEpoch(d.u64());
  return d.ok();
}

bool parse_outcome_resp(const proto::Frame& f, Outcome& out) {
  if (f.type != proto::FrameType::ACK && f.type != proto::FrameType::NACK) return false;
  proto::Decoder d(f.body.data(), f.body.size());
  out = static_cast<Outcome>(d.u8());
  return d.ok();
}

bool parse_query_resp(const proto::Frame& f, ReadinessState& state, DependencyNodeGeneration& gen,
                      std::vector<BlockingDependency>& blockers) {
  if (f.type != proto::FrameType::QUERY_RESP) return false;
  proto::Decoder d(f.body.data(), f.body.size());
  state = static_cast<ReadinessState>(d.u8());
  gen = DependencyNodeGeneration(d.u64());
  const std::uint32_t n = d.u32();
  for (std::uint32_t i = 0; i < n; ++i) {
    BlockingDependency b;
    b.edge_id = DependencyEdgeId(d.u64());
    b.producer = DependencyNodeId(d.u64());
    b.kind = static_cast<EdgeKind>(d.u8());
    b.optional = d.u8() != 0;
    b.reason = d.str();
    blockers.push_back(std::move(b));
  }
  return d.ok();
}

bool parse_persist_resp(const proto::Frame& f, Outcome& out, std::uint64_t& digest) {
  if (f.type != proto::FrameType::PERSIST_RESP) return false;
  proto::Decoder d(f.body.data(), f.body.size());
  out = static_cast<Outcome>(d.u8());
  digest = d.u64();
  return d.ok();
}

bool is_accepted(const proto::Frame& f) {
  Outcome o;
  return parse_outcome_resp(f, o) && o == Outcome::ACCEPTED;
}

// ---------------------------------------------------------------------------
// CoordinatorClient
// ---------------------------------------------------------------------------
bool CoordinatorClient::open(const std::string& host, std::uint16_t port, std::string* err) {
  return stream_.connect(host, port, err);
}
void CoordinatorClient::close() { stream_.close(); }
std::optional<proto::Frame> CoordinatorClient::exchange(const proto::Frame& req, std::string* err) {
  if (!stream_.write_frame(req, err)) return std::nullopt;
  return stream_.read_frame(err);
}

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------
namespace {
proto::Frame ack(Outcome o) {
  proto::Encoder enc; enc.u8(static_cast<std::uint8_t>(o));
  return proto::Frame(o == Outcome::ACCEPTED ? proto::FrameType::ACK : proto::FrameType::NACK, enc.take());
}
}  // namespace

std::optional<proto::Frame> Coordinator::handle(const proto::Frame& req) {
  using PT = proto::FrameType;
  const auto errFrame = [] { proto::Encoder e; return proto::Frame(PT::ERROR, e.take()); };
  switch (req.type) {
    case PT::HELLO_REQ: {
      proto::Encoder enc; enc.u64(graph_->current_epoch().value());
      return proto::Frame(PT::HELLO_RESP, enc.take());
    }
    case PT::REGISTER_REQ: {
      proto::Decoder d(req.body.data(), req.body.size());
      SourceId s(d.u64()); SourceBootId b(d.u64()); CoordinatorEpoch e(d.u64());
      if (!d.ok()) return errFrame();
      return ack(graph_->register_source(s, b, e));
    }
    case PT::PUBLISH_NODE_REQ: {
      proto::Decoder d(req.body.data(), req.body.size());
      DependencyNodeId n(d.u64());
      NodeKind k = static_cast<NodeKind>(d.u8());
      SourceId s(d.u64()); SourceBootId b(d.u64()); CoordinatorEpoch e(d.u64());
      std::string desc = d.str();
      if (!d.ok()) return errFrame();
      return ack(graph_->declare_node(n, k, std::move(desc), s, b, e));
    }
    case PT::PUBLISH_GEN_REQ: {
      proto::Decoder d(req.body.data(), req.body.size());
      DependencyNodeId n(d.u64());
      DependencyNodeGeneration g(d.u64());
      SourceId s(d.u64()); SourceBootId b(d.u64()); CoordinatorEpoch e(d.u64());
      if (!d.ok()) return errFrame();
      return ack(graph_->publish_generation(n, g, s, b, e));
    }
    case PT::ADD_EDGE_REQ: {
      proto::Decoder d(req.body.data(), req.body.size());
      Edge ed;
      ed.id = DependencyEdgeId(d.u64());
      ed.generation = DependencyEdgeGeneration(d.u64());
      ed.kind = static_cast<EdgeKind>(d.u8());
      ed.producer_id = DependencyNodeId(d.u64());
      ed.consumer_id = DependencyNodeId(d.u64());
      ed.bound_producer_generation = ProducerGeneration(d.u64());
      ed.bound_consumer_generation = ConsumerGeneration(d.u64());
      ed.active = d.u8() != 0;
      ed.predicate.generation_kind = static_cast<GenerationPredicateKind>(d.u8());
      ed.predicate.exact_generation = DependencyNodeGeneration(d.u64());
      ed.predicate.minimum_generation = DependencyNodeGeneration(d.u64());
      std::uint32_t cg = d.u32();
      for (std::uint32_t i = 0; i < cg; ++i) ed.predicate.compatible_generations.push_back(DependencyNodeGeneration(d.u64()));
      ed.predicate.required_readiness = static_cast<ReadinessState>(d.u8());
      ed.predicate.required_capability = d.str();
      ed.predicate.requires_resource_available = d.u8() != 0;
      ed.predicate.requires_fresh = d.u8() != 0;
      ed.predicate.requires_integrity = d.u8() != 0;
      ed.predicate.minimal_provenance_confidence = d.u64();
      ed.predicate.required_completion_state = d.str();
      ed.predicate.is_required = d.u8() != 0;
      ed.predicate.note = d.str();
      SourceId s(d.u64()); SourceBootId b(d.u64()); CoordinatorEpoch e(d.u64());
      if (!d.ok()) return errFrame();
      return ack(graph_->add_edge(ed, s, b, e));
    }
    case PT::QUERY_REQ: {
      proto::Decoder d(req.body.data(), req.body.size());
      DependencyNodeId n(d.u64());
      if (!d.ok()) return errFrame();
      ReadinessState st = graph_->readiness(n);
      DependencyNodeGeneration g = graph_->current_authority(n);
      std::vector<BlockingDependency> blockers = graph_->blockers(n);
      proto::Encoder enc;
      enc.u8(static_cast<std::uint8_t>(st));
      enc.u64(g.value());
      enc.u32(static_cast<std::uint32_t>(blockers.size()));
      for (const auto& b : blockers) {
        enc.u64(b.edge_id.value()); enc.u64(b.producer.value());
        enc.u8(static_cast<std::uint8_t>(b.kind)); enc.u8(b.optional ? 1 : 0); enc.str(b.reason);
      }
      return proto::Frame(PT::QUERY_RESP, enc.take());
    }
    case PT::INVALIDATE_REQ: {
      if (req.body.size() == 8) {
        proto::Decoder d(req.body.data(), req.body.size());
        CoordinatorEpoch e(d.u64());
        if (!d.ok()) return errFrame();
        return ack(graph_->advance_epoch(e));
      }
      proto::Decoder d(req.body.data(), req.body.size());
      DependencyNodeId n(d.u64());
      InvalidationReason r = static_cast<InvalidationReason>(d.u8());
      InvalidationMode m = static_cast<InvalidationMode>(d.u8());
      std::uint32_t depth = d.u32();
      bool prop = d.u8() != 0;
      bool degr = d.u8() != 0;
      SourceId s(d.u64()); SourceBootId b(d.u64()); CoordinatorEpoch e(d.u64());
      if (!d.ok()) return errFrame();
      InvalidationPolicy pol;
      pol.mode = m; pol.max_depth = depth; pol.propagate_through_optional = prop; pol.degrade_optional_dependents = degr;
      return ack(graph_->invalidate_node(n, r, pol, s, b, e));
    }
    case PT::MARK_RECOVERED_REQ: {
      proto::Decoder d(req.body.data(), req.body.size());
      DependencyNodeId n(d.u64());
      SourceId s(d.u64()); SourceBootId b(d.u64()); CoordinatorEpoch e(d.u64());
      if (!d.ok()) return errFrame();
      return ack(graph_->mark_recovered(n, s, b, e));
    }
    case PT::REMOVE_EDGE_REQ: {
      proto::Decoder d(req.body.data(), req.body.size());
      DependencyEdgeId id(d.u64());
      SourceId s(d.u64()); SourceBootId b(d.u64()); CoordinatorEpoch e(d.u64());
      if (!d.ok()) return errFrame();
      return ack(graph_->remove_edge(id, s, b, e));
    }
    case PT::PUBLISH_READINESS_REQ: {
      proto::Decoder d(req.body.data(), req.body.size());
      DependencyNodeId n(d.u64());
      SourceId s(d.u64()); SourceBootId b(d.u64()); CoordinatorEpoch e(d.u64());
      if (!d.ok()) return errFrame();
      return ack(graph_->publish_readiness(n, ReadinessState::READY, s, b, e));
    }
    case PT::PERSIST_REQ: {
      proto::Decoder d(req.body.data(), req.body.size());
      std::string path = d.str();
      if (!d.ok()) return errFrame();
      std::string err;
      Outcome o = Persistence::save_file(*graph_, path, &err);
      if (o != Outcome::ACCEPTED) return errFrame();
      proto::Encoder enc; enc.u8(static_cast<std::uint8_t>(o)); enc.u64(graph_->digest());
      return proto::Frame(PT::PERSIST_RESP, enc.take());
    }
    case PT::BYE:
      return std::nullopt;
    case PT::ACK:
    case PT::NACK:
    case PT::HELLO_RESP:
    case PT::PERSIST_RESP:
    case PT::QUERY_RESP:
    case PT::ERROR:
    default:
      return errFrame();
  }
}

void Coordinator::serve(std::uint16_t port, const std::atomic<bool>* stop) {
  proto::NetStream listener;
  std::string err;
  if (!listener.listen(port, &err)) return;
  while (stop == nullptr || !stop->load()) {
    proto::NetStream client;
    if (!listener.accept(client, &err)) continue;
    for (;;) {
      auto f = client.read_frame(&err);
      if (!f) break;
      auto resp = handle(*f);
      if (!resp) break;  // BYE
      if (!client.write_frame(*resp, &err)) break;
    }
    client.close();
  }
  listener.close();
}

}  // namespace dependency_fabric