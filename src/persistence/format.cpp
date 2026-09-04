// dependency_fabric::Persistence — strict, versioned, integrity-checked graph
// serialization.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dependency_fabric/persistence.hpp"

#include <fstream>
#include <limits>
#include <cstring>
#include <unordered_map>
#include <deque>

namespace dependency_fabric {

namespace {

constexpr std::size_t kMaxStringBytes = 1U << 20;    // 1 MiB per string
constexpr std::size_t kMaxCollectionItems = 1U << 24;

std::uint64_t fnv1a(const std::uint8_t* p, std::size_t n) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

class ByteWriter {
 public:
  void u8(std::uint8_t v) { buf_.push_back(v); }
  void u16(std::uint16_t v) { for (std::size_t i = 0; i < 2; ++i) buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu)); }
  void u32(std::uint32_t v) { for (std::size_t i = 0; i < 4; ++i) buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu)); }
  void u64(std::uint64_t v) { for (std::size_t i = 0; i < 8; ++i) buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu)); }
  void u8b(bool b) { u8(b ? 1u : 0u); }
  void str(const std::string& s) {
    u32(static_cast<std::uint32_t>(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
  }
  void bytes(const std::uint8_t* p, std::size_t n) { buf_.insert(buf_.end(), p, p + n); }
  const std::vector<std::uint8_t>& data() const { return buf_; }

 private:
  std::vector<std::uint8_t> buf_;
};

class ByteReader {
 public:
  ByteReader(const std::uint8_t* d, std::size_t n) : data_(d), size_(n) {}
  bool ok() const { return ok_; }
  std::size_t pos() const { return pos_; }

  std::uint8_t u8() {
    if (pos_ + 1 > size_) { ok_ = false; return 0; }
    return data_[pos_++];
  }
  std::uint16_t u16() {
    if (pos_ + 2 > size_) { ok_ = false; return 0; }
    std::uint16_t v = static_cast<std::uint16_t>(data_[pos_]) |
                      (static_cast<std::uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
  }
  std::uint32_t u32() {
    if (pos_ + 4 > size_) { ok_ = false; return 0; }
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) v |= (static_cast<std::uint32_t>(data_[pos_ + i]) << (8 * i));
    pos_ += 4;
    return v;
  }
  std::uint64_t u64() {
    if (pos_ + 8 > size_) { ok_ = false; return 0; }
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) v |= (static_cast<std::uint64_t>(data_[pos_ + i]) << (8 * i));
    pos_ += 8;
    return v;
  }
  bool u8b() { return u8() != 0; }
  std::string str() {
    std::uint32_t n = u32();
    if (!ok_) return {};
    if (n > kMaxStringBytes) { ok_ = false; return {}; }
    if (pos_ + n > size_) { ok_ = false; return {}; }
    std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
    pos_ += n;
    return s;
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

void write_node(ByteWriter& w, const Node& n) {
  w.u64(n.id.value());
  w.u8(static_cast<std::uint8_t>(n.kind));
  w.u8(static_cast<std::uint8_t>(n.state));
  w.u32(static_cast<std::uint32_t>(n.generation_history.size()));
  for (auto g : n.generation_history) w.u64(g.value());
  w.u64(n.current_generation.value());
  w.u64(n.artifact_generation.value());
  w.u64(n.state_generation.value());
  w.u64(n.resource_generation.value());
  w.u64(n.readiness_generation.value());
  w.u64(n.invalidation_generation.value());
  w.u64(n.recovery_generation.value());
  w.u64(n.policy_generation.value());
  w.u8b(n.static_durable_fact);
  w.u8b(n.dynamic_evidence);
  w.u8(static_cast<std::uint8_t>(n.recovered_evidence));
  w.u8b(n.was_ready_before_restart);
  w.u64(n.origin_source.value());
  w.u64(n.origin_boot.value());
  w.u32(static_cast<std::uint32_t>(n.superseded_generations.size()));
  for (auto g : n.superseded_generations) w.u64(g.value());
  w.u32(static_cast<std::uint32_t>(n.claimed_capabilities.size()));
  for (auto& c : n.claimed_capabilities) w.str(c);
  w.u8b(n.resource_available);
  w.u8b(n.integrity_valid);
  w.u8b(n.freshness_valid);
  w.u64(n.provenance_confidence);
  w.str(n.completion_state);
  w.u8(static_cast<std::uint8_t>(n.last_invalidation_reason));
  w.u8b(n.ever_invalidated);
  w.str(n.descriptor);
}

void write_edge(ByteWriter& w, const Edge& e) {
  w.u64(e.id.value());
  w.u64(e.generation.value());
  w.u8(static_cast<std::uint8_t>(e.kind));
  w.u64(e.producer_id.value());
  w.u64(e.consumer_id.value());
  w.u64(e.bound_producer_generation.value());
  w.u64(e.bound_consumer_generation.value());
  w.u8b(e.active);
  const DependencyPredicate& p = e.predicate;
  w.u8(static_cast<std::uint8_t>(p.generation_kind));
  w.u64(p.exact_generation.value());
  w.u64(p.minimum_generation.value());
  w.u32(static_cast<std::uint32_t>(p.compatible_generations.size()));
  for (auto g : p.compatible_generations) w.u64(g.value());
  w.u8(static_cast<std::uint8_t>(p.required_readiness));
  w.str(p.required_capability);
  w.u8b(p.requires_resource_available);
  w.u8b(p.requires_fresh);
  w.u8b(p.requires_integrity);
  w.u64(p.minimal_provenance_confidence);
  w.str(p.required_completion_state);
  w.u8b(p.is_required);
  w.str(p.note);
}

void write_set(ByteWriter& w, const DependencySet& s) {
  w.u64(s.id.value());
  w.u64(s.generation.value());
  w.u64(s.owner.value());
  w.u8(static_cast<std::uint8_t>(s.op));
  w.u32(s.at_least_n);
  w.u32(static_cast<std::uint32_t>(s.members.size()));
  for (auto m : s.members) w.u64(m.value());
  w.u8b(s.active);
}

void write_source(ByteWriter& w, const SourceRegistration& s) {
  w.u64(s.source.value());
  w.u64(s.boot.value());
  w.u64(s.epoch.value());
  w.u8b(s.active);
}

bool valid_node_kind(std::uint8_t v) { return v <= static_cast<std::uint8_t>(NodeKind::UNKNOWN); }
bool valid_edge_kind(std::uint8_t v) { return v <= static_cast<std::uint8_t>(EdgeKind::REQUIRES_GENERATION); }
bool valid_state(std::uint8_t v) { return v <= static_cast<std::uint8_t>(ReadinessState::RETIRED); }
bool valid_outcome(std::uint8_t) { return true; }
bool valid_gen_kind(std::uint8_t v) { return v <= static_cast<std::uint8_t>(GenerationPredicateKind::NONE); }
bool valid_set_op(std::uint8_t v) { return v <= static_cast<std::uint8_t>(SetOperator::OPTIONAL_GROUP); }
bool valid_dyn_evidence(std::uint8_t v) { return v <= static_cast<std::uint8_t>(DynamicEvidence::FRESH); }
bool valid_inv_reason(std::uint8_t v) { return v <= static_cast<std::uint8_t>(InvalidationReason::SUPERSEDED); }


}  // namespace

std::optional<std::vector<std::uint8_t>> Persistence::save(const Graph& g, std::string* error) {
  std::shared_lock lock(g.mutex_);
  ByteWriter body;
  body.u64(g.epoch_.value());
  body.u32(static_cast<std::uint32_t>(g.sources_.size()));
  for (const auto& kv : g.sources_) write_source(body, kv.second);
  body.u32(static_cast<std::uint32_t>(g.nodes_.size()));
  for (const auto& kv : g.nodes_) write_node(body, kv.second);
  body.u32(static_cast<std::uint32_t>(g.edges_.size()));
  for (const auto& kv : g.edges_) write_edge(body, kv.second);
  body.u32(static_cast<std::uint32_t>(g.sets_.size()));
  for (const auto& kv : g.sets_) write_set(body, kv.second);

  const auto& b = body.data();
  const std::uint64_t body_hash = fnv1a(b.data(), b.size());

  ByteWriter out;
  out.u32(PersistenceFormat::kMagic);
  out.u16(PersistenceFormat::kVersion);
  out.u16(0); // flags
  out.u64(static_cast<std::uint64_t>(b.size()));
  out.u64(body_hash);
  out.bytes(b.data(), b.size());
  if (error) error->clear();
  return out.data();
}

Outcome Persistence::load(const std::vector<std::uint8_t>& bytes, Graph* g, std::string* error) {
  const auto fail = [&](const std::string& msg) -> Outcome {
    if (error) *error = msg;
    return Outcome::REJECT_MALFORMED;
  };
  if (!g) return fail("null graph");
  if (bytes.size() < 8 + 8 + 8 + 8) return fail("payload too small");

  ByteReader r(bytes.data(), bytes.size());
  const std::uint32_t magic = r.u32();
  const std::uint16_t version = r.u16();
  const std::uint16_t flags = r.u16();
  const std::uint64_t body_len = r.u64();
  const std::uint64_t body_hash = r.u64();
  (void)flags;
  if (!r.ok()) return fail("truncated header");
  if (magic != PersistenceFormat::kMagic) return fail("bad magic");
  if (version != PersistenceFormat::kVersion) return fail("unsupported version");
  if (body_len > bytes.size() || r.pos() + body_len != bytes.size()) return fail("body length mismatch / trailing garbage");
  const std::uint8_t* bodyp = bytes.data() + r.pos();
  if (fnv1a(bodyp, static_cast<std::size_t>(body_len)) != body_hash) return fail("content digest mismatch (corruption)");

  ByteReader b(bodyp, static_cast<std::size_t>(body_len));

  const std::uint64_t epoch = b.u64();
  const std::uint32_t source_count = b.u32();
  if (source_count > kMaxCollectionItems) return fail("source count too large");
  std::unordered_map<std::uint64_t, SourceRegistration> sources;
  for (std::uint32_t i = 0; i < source_count; ++i) {
    SourceId sid(b.u64());
    SourceBootId boot(b.u64());
    CoordinatorEpoch ep(b.u64());
    bool active = b.u8b();
    if (!b.ok()) return fail("truncated source");
    if (!sid.valid()) return fail("invalid source id");
    if (sources.count(sid.value())) return fail("duplicate source id");
    sources[sid.value()] = SourceRegistration{sid, boot, ep, active};
  }

  const std::uint32_t node_count = b.u32();
  if (node_count > kMaxCollectionItems) return fail("node count too large");
  std::vector<Node> nodes;
  nodes.reserve(node_count);
  for (std::uint32_t i = 0; i < node_count; ++i) {
    Node n(DependencyNodeId(b.u64()));
    std::uint8_t k = b.u8();
    std::uint8_t st = b.u8();
    if (!valid_node_kind(k)) return fail("invalid node kind enum");
    if (!valid_state(st)) return fail("invalid node state enum");
    n.kind = static_cast<NodeKind>(k);
    n.state = static_cast<ReadinessState>(st);
    const std::uint32_t ghist = b.u32();
    if (ghist > node_count * 2 + 16) return fail("node generation history too large");
    std::uint64_t prev = 0;
    bool first = true;
    for (std::uint32_t gi = 0; gi < ghist; ++gi) {
      std::uint64_t genv = b.u64();
      if (!first && genv <= prev) return fail("generation regression in node history");
      prev = genv;
      first = false;
      n.generation_history.push_back(DependencyNodeGeneration(genv));
    }
    n.current_generation = DependencyNodeGeneration(b.u64());
    n.artifact_generation = ArtifactGeneration(b.u64());
    n.state_generation = StateGeneration(b.u64());
    n.resource_generation = ResourceGeneration(b.u64());
    n.readiness_generation = ReadinessGeneration(b.u64());
    n.invalidation_generation = InvalidationGeneration(b.u64());
    n.recovery_generation = RecoveryGeneration(b.u64());
    n.policy_generation = PolicyGeneration(b.u64());
    n.static_durable_fact = b.u8b();
    n.dynamic_evidence = b.u8b();
    std::uint8_t de = b.u8();
    if (!valid_dyn_evidence(de)) return fail("invalid dynamic evidence enum");
    n.recovered_evidence = static_cast<DynamicEvidence>(de);
    n.was_ready_before_restart = b.u8b();
    n.origin_source = SourceId(b.u64());
    n.origin_boot = SourceBootId(b.u64());
    const std::uint32_t sup = b.u32();
    if (sup > node_count * 2 + 16) return fail("superseded generation count too large");
    for (std::uint32_t si = 0; si < sup; ++si) n.superseded_generations.push_back(DependencyNodeGeneration(b.u64()));
    const std::uint32_t capn = b.u32();
    if (capn > 1024) return fail("capability count too large");
    for (std::uint32_t ci = 0; ci < capn; ++ci) n.claimed_capabilities.push_back(b.str());
    n.resource_available = b.u8b();
    n.integrity_valid = b.u8b();
    n.freshness_valid = b.u8b();
    n.provenance_confidence = b.u64();
    n.completion_state = b.str();
    std::uint8_t lr = b.u8();
    if (!valid_inv_reason(lr)) return fail("invalid invalidation reason enum");
    n.last_invalidation_reason = static_cast<InvalidationReason>(lr);
    n.ever_invalidated = b.u8b();
    n.descriptor = b.str();
    if (!b.ok()) return fail("truncated node");
    if (!n.id.valid()) return fail("invalid node id");
    if (!n.generation_history.empty()) {
      if (n.current_generation.value() != n.generation_history.back().value()) return fail("current generation does not match history");
    } else if (n.current_generation.valid()) {
      return fail("current generation set without history");
    }
    nodes.push_back(std::move(n));
    (void)first;
  }

  const std::uint32_t edge_count = b.u32();
  if (edge_count > kMaxCollectionItems) return fail("edge count too large");
  std::vector<Edge> edges;
  edges.reserve(edge_count);
  for (std::uint32_t i = 0; i < edge_count; ++i) {
    Edge e;
    e.id = DependencyEdgeId(b.u64());
    e.generation = DependencyEdgeGeneration(b.u64());
    std::uint8_t k = b.u8();
    if (!valid_edge_kind(k)) return fail("invalid edge kind enum");
    e.kind = static_cast<EdgeKind>(k);
    e.producer_id = DependencyNodeId(b.u64());
    e.consumer_id = DependencyNodeId(b.u64());
    e.bound_producer_generation = ProducerGeneration(b.u64());
    e.bound_consumer_generation = ConsumerGeneration(b.u64());
    e.active = b.u8b();
    e.predicate.generation_kind = static_cast<GenerationPredicateKind>(b.u8());
    if (!valid_gen_kind(static_cast<std::uint8_t>(e.predicate.generation_kind))) return fail("invalid generation predicate enum");
    e.predicate.exact_generation = DependencyNodeGeneration(b.u64());
    e.predicate.minimum_generation = DependencyNodeGeneration(b.u64());
    const std::uint32_t cg = b.u32();
    if (cg > 1024) return fail("compatible generation count too large");
    for (std::uint32_t ci = 0; ci < cg; ++ci) e.predicate.compatible_generations.push_back(DependencyNodeGeneration(b.u64()));
    e.predicate.required_readiness = static_cast<ReadinessState>(b.u8());
    if (!valid_state(static_cast<std::uint8_t>(e.predicate.required_readiness))) return fail("invalid required readiness enum");
    e.predicate.required_capability = b.str();
    e.predicate.requires_resource_available = b.u8b();
    e.predicate.requires_fresh = b.u8b();
    e.predicate.requires_integrity = b.u8b();
    e.predicate.minimal_provenance_confidence = b.u64();
    e.predicate.required_completion_state = b.str();
    e.predicate.is_required = b.u8b();
    e.predicate.note = b.str();
    if (!b.ok()) return fail("truncated edge");
    if (!e.id.valid()) return fail("invalid edge id");
    if (e.producer_id.value() == e.consumer_id.value()) return fail("illegal self-cycle");
    edges.push_back(std::move(e));
  }

  const std::uint32_t set_count = b.u32();
  if (set_count > kMaxCollectionItems) return fail("set count too large");
  std::vector<DependencySet> sets;
  sets.reserve(set_count);
  for (std::uint32_t i = 0; i < set_count; ++i) {
    DependencySet s;
    s.id = DependencySetId(b.u64());
    s.generation = DependencySetGeneration(b.u64());
    s.owner = DependencyNodeId(b.u64());
    std::uint8_t op = b.u8();
    if (!valid_set_op(op)) return fail("invalid set operator enum");
    s.op = static_cast<SetOperator>(op);
    s.at_least_n = b.u32();
    const std::uint32_t mn = b.u32();
    if (mn > 1048576) return fail("set member count too large");
    for (std::uint32_t mi = 0; mi < mn; ++mi) s.members.push_back(DependencyEdgeId(b.u64()));
    s.active = b.u8b();
    if (!b.ok()) return fail("truncated set");
    if (!s.id.valid()) return fail("invalid set id");
    sets.push_back(std::move(s));
  }
  if (!b.ok()) return fail("truncated body");

  // Acyclic local check.
  {
    const std::uint64_t c1 = b.pos();
    (void)c1;
  }

  // Build id sets first so endpoint checks are O(1).
  std::unordered_map<std::uint64_t, bool> node_ids;
  for (const auto& n : nodes) {
    if (node_ids.count(n.id.value())) return fail("duplicate node id");
    node_ids[n.id.value()] = true;
  }
  std::unordered_map<std::uint64_t, bool> edge_ids;
  for (const auto& e : edges) {
    if (edge_ids.count(e.id.value())) return fail("duplicate edge id");
    edge_ids[e.id.value()] = true;
  }
  const auto has_node = [&](std::uint64_t idv) { return node_ids.count(idv) != 0; };
  const auto has_edge = [&](std::uint64_t idv) { return edge_ids.count(idv) != 0; };

  // Validate edge endpoints and sets.
  for (const auto& e : edges) {
    if (!has_node(e.producer_id.value())) return fail("broken edge endpoint (producer)");
    if (!has_node(e.consumer_id.value())) return fail("broken edge endpoint (consumer)");
  }
  for (const auto& s : sets) {
    if (!has_node(s.owner.value())) return fail("set owner missing");
    if (s.members.empty() && s.op != SetOperator::OPTIONAL_GROUP) return fail("empty non-optional set");
    for (auto m : s.members) {
      if (!has_edge(m.value())) return fail("set member edge missing");
    }
  }

  // Build a temp graph-like structure to verify acyclicity before committing.
  Graph tmp;
  std::unordered_map<std::uint64_t, std::uint32_t> tmp_indeg;
  {
    std::unique_lock lk(tmp.mutex_);
    tmp.epoch_ = CoordinatorEpoch(epoch);
    for (auto& kv : sources) tmp.sources_[SourceId(kv.first)] = kv.second;
    for (auto& n : nodes) tmp.nodes_[n.id] = std::move(n);
    for (auto& e : edges) tmp.edges_[e.id] = std::move(e);
    for (auto& s : sets) tmp.sets_[s.id] = std::move(s);
    tmp.rebuild_indexes_locked();
    for (const auto& kv : tmp.nodes_) tmp_indeg[kv.first.value()] = 0;
    for (const auto& kv : tmp.edges_) {
      if (kv.second.active) tmp_indeg[kv.second.consumer_id.value()]++;
    }
    std::deque<std::uint64_t> tq;
    for (const auto& kv : tmp_indeg) if (kv.second == 0) tq.push_back(kv.first);
    std::size_t tvisited = 0;
    while (!tq.empty()) {
      std::uint64_t cur = tq.front(); tq.pop_front();
      ++tvisited;
      auto oe = tmp.out_edges_.find(DependencyNodeId(cur));
      if (oe == tmp.out_edges_.end()) continue;
      for (auto eid : oe->second) {
        auto eit = tmp.edges_.find(eid);
        if (eit == tmp.edges_.end() || !eit->second.active) continue;
        auto cid = eit->second.consumer_id.value();
        if (--tmp_indeg[cid] == 0) tq.push_back(cid);
      }
    }
    if (tvisited != tmp.nodes_.size()) return fail("cyclic DAG reconstruction");
  }

  // Commit into the output graph.
  {
    std::unique_lock lk(g->mutex_);
    g->epoch_ = CoordinatorEpoch(epoch);
    g->sources_ = tmp.sources_;
    g->nodes_ = std::move(tmp.nodes_);
    g->edges_ = std::move(tmp.edges_);
    g->sets_ = std::move(tmp.sets_);
    g->rebuild_indexes_locked();
    // Conservative recovery: dynamic evidence that was fresh before restart is
    // demoted to REVALIDATION_REQUIRED; static durable facts remain durable.
    for (auto& kv : g->nodes_) {
      Node& n = kv.second;
      if (n.dynamic_evidence) {
        n.recovered_evidence = DynamicEvidence::REVALIDATION_REQUIRED;
        n.state = ReadinessState::REVALIDATION_REQUIRED;
        n.was_ready_before_restart = true;
      } else {
        n.recovered_evidence = DynamicEvidence::FRESH;
      }
    }
    g->recompute_all_locked();
  }

  if (error) error->clear();
  return Outcome::ACCEPTED;
}

Outcome Persistence::save_file(const Graph& graph, const std::string& path, std::string* error) {
  auto bytes = save(graph, error);
  if (!bytes) return Outcome::REJECT_MALFORMED;
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) { if (error) *error = "cannot open " + path; return Outcome::REJECT_MALFORMED; }
  f.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
  if (!f) { if (error) *error = "write failed for " + path; return Outcome::REJECT_MALFORMED; }
  return Outcome::ACCEPTED;
}

Outcome Persistence::load_file(const std::string& path, Graph* graph, std::string* error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) { if (error) *error = "cannot open " + path; return Outcome::REJECT_MALFORMED; }
  const auto end = f.tellg();
  if (end <= 0) { if (error) *error = "empty file " + path; return Outcome::REJECT_MALFORMED; }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
  f.seekg(0, std::ios::beg);
  f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!f) { if (error) *error = "read failed for " + path; return Outcome::REJECT_MALFORMED; }
  return load(bytes, graph, error);
}

// ---------------------------------------------------------------------------
// Graph::serialize and Graph::digest delegate to Persistence.
// ---------------------------------------------------------------------------
std::optional<std::vector<std::uint8_t>> Graph::serialize() const {
  std::string err;
  return Persistence::save(*this, &err);
}

std::uint64_t Graph::digest() const {
  auto bytes = serialize();
  if (!bytes) return 0;
  return fnv1a(bytes->data(), bytes->size());
}

}  // namespace dependency_fabric