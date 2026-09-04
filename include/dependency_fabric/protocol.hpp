// dependency_fabric::protocol — framed, checksummed TCP protocol.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Frames carry a versioned header (magic, version, type, body length,
// checksum) and a bounded body. Decoding is bounded and checksum-verified;
// partial reads/writes are handled by looping until complete. The wire layer
// is transport-agnostic behind NetByteStream so tests can drive it over
// loopback sockets.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <optional>

namespace dependency_fabric {

namespace proto {

constexpr std::uint32_t kMagic = 0x44463132u;        // "DF12"
constexpr std::uint16_t kVersion = 1;
constexpr std::uint32_t kMaxBodyBytes = 1U << 24;    // 16 MiB hard cap
constexpr std::size_t kHeaderBytes = 4 + 2 + 2 + 4 + 4;  // magic+ver+type+len+checksum

enum class FrameType : std::uint16_t {
  HELLO_REQ = 1,
  HELLO_RESP = 2,
  REGISTER_REQ = 3,
  ACK = 4,           // generic outcome ack
  NACK = 5,          // rejection with reason
  PUBLISH_NODE_REQ = 6,
  PUBLISH_GEN_REQ = 7,
  ADD_EDGE_REQ = 8,
  QUERY_REQ = 9,
  QUERY_RESP = 10,
  INVALIDATE_REQ = 11,
  MARK_RECOVERED_REQ = 12,
  PERSIST_REQ = 13,
  PERSIST_RESP = 14,
  REMOVE_EDGE_REQ = 15,
  PUBLISH_READINESS_REQ = 16,
  BYE = 17,
  ERROR = 18,
};

struct Frame {
  FrameType type = FrameType::ERROR;
  std::vector<std::uint8_t> body;
  Frame() = default;
  Frame(FrameType t, std::vector<std::uint8_t> b) : type(t), body(std::move(b)) {}
};

// ---------------------------------------------------------------------------
// Encoder / decoder (little-endian, bounded).
// ---------------------------------------------------------------------------
class Encoder {
 public:
  void u8(std::uint8_t v);
  void u16(std::uint16_t v);
  void u32(std::uint32_t v);
  void u64(std::uint64_t v);
  void bytes(const std::uint8_t* p, std::size_t n);
  void str(const std::string& s);
  const std::vector<std::uint8_t>& data() const { return buf_; }
  std::vector<std::uint8_t> take() { return std::move(buf_); }

 private:
  std::vector<std::uint8_t> buf_;
};

class Decoder {
 public:
  Decoder(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}
  bool ok() const { return ok_; }
  std::size_t remaining() const { return n_ - pos_; }
  std::uint8_t u8();
  std::uint16_t u16();
  std::uint32_t u32();
  std::uint64_t u64();
  std::string str();
  void skip(std::size_t n);
  const std::uint8_t* raw(std::size_t n);

 private:
  const std::uint8_t* p_;
  std::size_t n_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

// Compute the 32-bit checksum over a body.
std::uint32_t checksum32(const std::uint8_t* p, std::size_t n);

// Encode a frame (header + body) into a byte vector.
std::optional<std::vector<std::uint8_t>> encode_frame(const Frame& frame);

// Decode a frame from a byte buffer (must contain exactly one frame).
std::optional<Frame> decode_frame(const std::uint8_t* p, std::size_t n);

// ---------------------------------------------------------------------------
// Transport (loopback TCP) — Windows (Winsock) and POSIX (BSD sockets).
// ---------------------------------------------------------------------------
class NetStream {
 public:
  NetStream() = default;
  ~NetStream();
  NetStream(const NetStream&) = delete;
  NetStream& operator=(const NetStream&) = delete;

  bool ok() const { return fd_ >= 0; }
  // Client connect to host:port.
  bool connect(const std::string& host, std::uint16_t port, std::string* err);
  // Server: listen on port, accept one connection.
  bool listen(std::uint16_t port, std::string* err);
  bool accept(NetStream& out, std::string* err);
  void close();
  // Write all bytes (loops on partial writes).
  bool write_all(const std::uint8_t* p, std::size_t n, std::string* err);
  // Read exactly n bytes (loops on partial reads).
  bool read_exact(std::uint8_t* out, std::size_t n, std::string* err);
  // Read one frame.
  std::optional<Frame> read_frame(std::string* err);
  // Write one frame.
  bool write_frame(const Frame& f, std::string* err);

  // Port used (useful when binding to ephemeral port 0).
  std::uint16_t port() const { return port_; }

 private:
  explicit NetStream(intptr_t fd) : fd_(fd) {}
  intptr_t fd_ = -1;
  std::uint16_t port_ = 0;
};

std::uint16_t find_free_port();

}  // namespace proto
}  // namespace dependency_fabric