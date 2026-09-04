// dependency_fabric::protocol — framed, checksummed TCP protocol implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dependency_fabric/protocol.hpp"

#include <cstring>
#include <atomic>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socklen_t = int;
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dependency_fabric {
namespace proto {

namespace {
#ifdef _WIN32
std::atomic<bool> g_winsock_ready{false};
bool ensure_winsock() {
  if (g_winsock_ready.load()) return true;
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
  g_winsock_ready.store(true);
  return true;
}
const char* last_error_str() {
  static thread_local char buf[256];
#ifdef _WIN32
  DWORD e = WSAGetLastError();
  std::snprintf(buf, sizeof(buf), "Winsock error %lu", static_cast<unsigned long>(e));
#else
  std::snprintf(buf, sizeof(buf), "socket error %d", errno);
#endif
  return buf;
}
#endif
}  // namespace

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------
void Encoder::u8(std::uint8_t v) { buf_.push_back(v); }
void Encoder::u16(std::uint16_t v) {
  for (std::size_t i = 0; i < 2; ++i) buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
}
void Encoder::u32(std::uint32_t v) {
  for (std::size_t i = 0; i < 4; ++i) buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
}
void Encoder::u64(std::uint64_t v) {
  for (std::size_t i = 0; i < 8; ++i) buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
}
void Encoder::bytes(const std::uint8_t* p, std::size_t n) { buf_.insert(buf_.end(), p, p + n); }
void Encoder::str(const std::string& s) {
  u32(static_cast<std::uint32_t>(s.size()));
  buf_.insert(buf_.end(), s.begin(), s.end());
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------
std::uint8_t Decoder::u8() {
  if (pos_ + 1 > n_) { ok_ = false; return 0; }
  return p_[pos_++];
}
std::uint16_t Decoder::u16() {
  if (pos_ + 2 > n_) { ok_ = false; return 0; }
  std::uint16_t v = static_cast<std::uint16_t>(p_[pos_]) |
                    (static_cast<std::uint16_t>(p_[pos_ + 1]) << 8);
  pos_ += 2;
  return v;
}
std::uint32_t Decoder::u32() {
  if (pos_ + 4 > n_) { ok_ = false; return 0; }
  std::uint32_t v = 0;
  for (std::size_t i = 0; i < 4; ++i) v |= (static_cast<std::uint32_t>(p_[pos_ + i]) << (8 * i));
  pos_ += 4;
  return v;
}
std::uint64_t Decoder::u64() {
  if (pos_ + 8 > n_) { ok_ = false; return 0; }
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) v |= (static_cast<std::uint64_t>(p_[pos_ + i]) << (8 * i));
  pos_ += 8;
  return v;
}
std::string Decoder::str() {
  std::uint32_t len = u32();
  if (!ok_) return {};
  if (len > kMaxBodyBytes || pos_ + len > n_) { ok_ = false; return {}; }
  std::string s(reinterpret_cast<const char*>(p_ + pos_), len);
  pos_ += len;
  return s;
}
void Decoder::skip(std::size_t n) {
  if (pos_ + n > n_) { ok_ = false; return; }
  pos_ += n;
}
const std::uint8_t* Decoder::raw(std::size_t n) {
  if (pos_ + n > n_) { ok_ = false; return nullptr; }
  const std::uint8_t* r = p_ + pos_;
  pos_ += n;
  return r;
}

std::uint32_t checksum32(const std::uint8_t* p, std::size_t n) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ULL;
  }
  return static_cast<std::uint32_t>(h ^ (h >> 32));
}

std::optional<std::vector<std::uint8_t>> encode_frame(const Frame& frame) {
  if (frame.body.size() > kMaxBodyBytes) return std::nullopt;
  Encoder enc;
  enc.u32(kMagic);
  enc.u16(kVersion);
  enc.u16(static_cast<std::uint16_t>(frame.type));
  enc.u32(static_cast<std::uint32_t>(frame.body.size()));
  enc.u32(checksum32(frame.body.data(), frame.body.size()));
  enc.bytes(frame.body.data(), frame.body.size());
  return enc.take();
}

std::optional<Frame> decode_frame(const std::uint8_t* p, std::size_t n) {
  if (n < kHeaderBytes) return std::nullopt;
  Decoder d(p, n);
  const std::uint32_t magic = d.u32();
  const std::uint16_t version = d.u16();
  const std::uint16_t typev = d.u16();
  const std::uint32_t len = d.u32();
  const std::uint32_t cs = d.u32();
  if (!d.ok()) return std::nullopt;
  if (magic != kMagic) return std::nullopt;
  if (version != kVersion) return std::nullopt;
  if (len > kMaxBodyBytes) return std::nullopt;
  if (n != static_cast<std::size_t>(kHeaderBytes) + len) return std::nullopt;
  const std::uint8_t* body = p + kHeaderBytes;
  if (checksum32(body, len) != cs) return std::nullopt;
  Frame f;
  f.type = static_cast<FrameType>(typev);
  f.body.assign(body, body + len);
  return f;
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------
NetStream::~NetStream() { close(); }

void NetStream::close() {
  if (fd_ < 0) return;
#ifdef _WIN32
  closesocket(static_cast<SOCKET>(fd_));
#else
  ::close(static_cast<int>(fd_));
#endif
  fd_ = -1;
}

bool NetStream::connect(const std::string& host, std::uint16_t port, std::string* err) {
#ifdef _WIN32
  if (!ensure_winsock()) { if (err) *err = "WSAStartup failed"; return false; }
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) { if (err) *err = last_error_str(); return false; }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
  if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    if (err) *err = last_error_str();
    closesocket(s);
    return false;
  }
  fd_ = static_cast<intptr_t>(s);
  port_ = port;
  return true;
#else
  int s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { if (err) *err = "socket failed"; return false; }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
  if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (err) *err = "connect failed";
    ::close(s);
    return false;
  }
  fd_ = s;
  port_ = port;
  return true;
#endif
}

bool NetStream::listen(std::uint16_t port, std::string* err) {
#ifdef _WIN32
  if (!ensure_winsock()) { if (err) *err = "WSAStartup failed"; return false; }
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) { if (err) *err = last_error_str(); return false; }
  int opt = 1;
  ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    if (err) *err = last_error_str();
    closesocket(s);
    return false;
  }
  if (::listen(s, 8) == SOCKET_ERROR) {
    if (err) *err = last_error_str();
    closesocket(s);
    return false;
  }
  sockaddr_in bound{};
  int blen = sizeof(bound);
  ::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &blen);
  fd_ = static_cast<intptr_t>(s);
  port_ = ntohs(bound.sin_port);
  return true;
#else
  int s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { if (err) *err = "socket failed"; return false; }
  int opt = 1;
  ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (err) *err = "bind failed";
    ::close(s);
    return false;
  }
  if (::listen(s, 8) != 0) { if (err) *err = "listen failed"; ::close(s); return false; }
  sockaddr_in bound{};
  socklen_t blen = sizeof(bound);
  ::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &blen);
  fd_ = s;
  port_ = ntohs(bound.sin_port);
  return true;
#endif
}

bool NetStream::accept(NetStream& out, std::string* err) {
#ifdef _WIN32
  sockaddr_in addr{};
  int alen = sizeof(addr);
  SOCKET c = ::accept(static_cast<SOCKET>(fd_), reinterpret_cast<sockaddr*>(&addr), &alen);
  if (c == INVALID_SOCKET) { if (err) *err = last_error_str(); return false; }
  out.fd_ = static_cast<intptr_t>(c);
  out.port_ = ntohs(addr.sin_port);
  return true;
#else
  sockaddr_in addr{};
  socklen_t alen = sizeof(addr);
  int c = ::accept(static_cast<int>(fd_), reinterpret_cast<sockaddr*>(&addr), &alen);
  if (c < 0) { if (err) *err = "accept failed"; return false; }
  out.fd_ = c;
  out.port_ = ntohs(addr.sin_port);
  return true;
#endif
}

bool NetStream::write_all(const std::uint8_t* p, std::size_t n, std::string* err) {
  std::size_t sent = 0;
  while (sent < n) {
#ifdef _WIN32
    int r = ::send(static_cast<SOCKET>(fd_), reinterpret_cast<const char*>(p + sent),
                   static_cast<int>(n - sent), 0);
#else
    int r = static_cast<int>(::send(static_cast<int>(fd_), p + sent, n - sent, MSG_NOSIGNAL));
#endif
    if (r <= 0) { if (err) *err = "send failed"; return false; }
    sent += static_cast<std::size_t>(r);
  }
  return true;
}

bool NetStream::read_exact(std::uint8_t* out, std::size_t n, std::string* err) {
  std::size_t got = 0;
  while (got < n) {
#ifdef _WIN32
    int r = ::recv(static_cast<SOCKET>(fd_), reinterpret_cast<char*>(out + got),
                   static_cast<int>(n - got), 0);
#else
    int r = static_cast<int>(::recv(static_cast<int>(fd_), out + got, n - got, 0));
#endif
    if (r == 0) { if (err) *err = "connection closed"; return false; }
    if (r < 0) { if (err) *err = "recv failed"; return false; }
    got += static_cast<std::size_t>(r);
  }
  return true;
}

std::optional<Frame> NetStream::read_frame(std::string* err) {
  std::uint8_t header[kHeaderBytes];
  if (!read_exact(header, kHeaderBytes, err)) return std::nullopt;
  Decoder hd(header, kHeaderBytes);
  const std::uint32_t magic = hd.u32();
  const std::uint16_t version = hd.u16();
  const std::uint16_t typev = hd.u16();
  const std::uint32_t len = hd.u32();
  const std::uint32_t cs = hd.u32();
  if (!hd.ok() || magic != kMagic || version != kVersion) {
    if (err) *err = "bad frame header";
    return std::nullopt;
  }
  if (len > kMaxBodyBytes) { if (err) *err = "frame too large"; return std::nullopt; }
  std::vector<std::uint8_t> body(len);
  if (len > 0 && !read_exact(body.data(), len, err)) return std::nullopt;
  if (checksum32(body.data(), len) != cs) { if (err) *err = "frame checksum mismatch"; return std::nullopt; }
  Frame f;
  f.type = static_cast<FrameType>(typev);
  f.body = std::move(body);
  return f;
}

bool NetStream::write_frame(const Frame& f, std::string* err) {
  auto bytes = encode_frame(f);
  if (!bytes) { if (err) *err = "frame too large or invalid"; return false; }
  return write_all(bytes->data(), bytes->size(), err);
}

std::uint16_t find_free_port() {
  NetStream s;
  std::string err;
  if (!s.listen(0, &err)) return 0;
  return s.port();
}

}  // namespace proto
}  // namespace dependency_fabric