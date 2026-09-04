// dependency_fabric::df_app — multiprocess proof (coordinator, workers, consumer,
// orchestrator) over real loopback TCP and real OS processes.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "dependency_fabric/df.hpp"
#include "dependency_fabric/coordinator.hpp"
#include "dependency_fabric/protocol.hpp"
#include "dependency_fabric/persistence.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace dependency_fabric;

namespace {

const SourceId SRC_A(1), SRC_B(2), SRC_C(3), SRC_ORCH(4);
const SourceBootId BOOT_1(1), BOOT_2(2);
const CoordinatorEpoch EPOCH_1(1), EPOCH_2(2);

inline void proof_log(const std::string& msg) {
  std::fprintf(stderr, "[proof] %s\n", msg.c_str());
  std::fflush(stderr);
}
#define PROOF_STEP(msg) proof_log(std::string(msg))

// Build a double-quoted shell-safe path fragment without backslash-quote escapes.
std::string qstr(const std::string& s) {
  std::string r;
  r.push_back('"');
  r += s;
  r.push_back('"');
  return r;
}

std::string marker_file(const std::string& dir, const std::string& name) { return dir + "/" + name; }

void write_file(const std::string& path, const std::string& content) {
  std::ofstream f(path, std::ios::trunc);
  f << content;
}

bool file_exists(const std::string& path) {
  std::ifstream f(path);
  return f.good();
}

std::string read_file(const std::string& path) {
  std::ifstream f(path);
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return s;
}

void wait_for(const std::string& path, int timeout_ms) {
  for (int i = 0; i < timeout_ms; ++i) {
    if (file_exists(path)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  if (!file_exists(path)) {
    proof_log(std::string("wait_for timeout: ") + path);
  }
}

Edge make_edge(DependencyEdgeId id, DependencyNodeId producer, DependencyNodeId consumer,
               DependencyNodeGeneration pinned) {
  Edge e;
  e.id = id;
  e.generation = DependencyEdgeGeneration(1);
  e.kind = EdgeKind::REQUIRES_GENERATION;
  e.producer_id = producer;
  e.consumer_id = consumer;
  e.predicate.generation_kind = GenerationPredicateKind::EXACT;
  e.predicate.exact_generation = pinned;
  e.predicate.is_required = true;
  return e;
}

bool rpc(CoordinatorClient& c, const proto::Frame& req, proto::Frame& resp, std::string* err) {
  auto r = c.exchange(req, err);
  if (!r) return false;
  resp = *r;
  return true;
}

bool rpc_outcome(CoordinatorClient& c, const proto::Frame& req, Outcome& out, std::string* err) {
  proto::Frame resp;
  if (!rpc(c, req, resp, err)) return false;
  return parse_outcome_resp(resp, out);
}

bool query_readiness(CoordinatorClient& c, DependencyNodeId node, ReadinessState& st) {
  proto::Frame resp;
  std::string err;
  if (!rpc(c, query_req(node), resp, &err)) return false;
  DependencyNodeGeneration gen;
  std::vector<BlockingDependency> bl;
  return parse_query_resp(resp, st, gen, bl);
}

int run_coordinator(uint16_t port, const std::string& ready, const std::string& load) {
  Graph graph;
  if (!load.empty()) {
    std::string err;
    if (Persistence::load_file(load, &graph, &err) != Outcome::ACCEPTED) {
      proof_log("coordinator load failed: " + err);
    }
  }
  Coordinator coord(&graph);
  std::atomic<bool> stop{false};
  write_file(ready, "ready");
  coord.serve(port, &stop);
  return 0;
}

int run_worker_a(uint16_t port, const std::string& markerDir) {
  CoordinatorClient c; std::string err;
  if (!c.open("127.0.0.1", port, &err)) return 1;
  proto::Frame resp; Outcome o;
  rpc(c, hello_req(), resp, &err);
  rpc_outcome(c, register_req(SRC_A, BOOT_1, EPOCH_1), o, &err);
  rpc_outcome(c, publish_node_req(DependencyNodeId(1), NodeKind::MODEL, "model", SRC_A, BOOT_1, EPOCH_1), o, &err);
  rpc_outcome(c, publish_gen_req(DependencyNodeId(1), DependencyNodeGeneration(1), SRC_A, BOOT_1, EPOCH_1), o, &err);
  rpc(c, bye(), resp, &err);
  c.close();
  write_file(marker_file(markerDir, "a_published.flag"), "A1");
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(3600));
}

int run_worker_b(uint16_t port, const std::string& markerDir) {
  CoordinatorClient c; std::string err;
  if (!c.open("127.0.0.1", port, &err)) return 1;
  proto::Frame resp; Outcome o;
  rpc(c, hello_req(), resp, &err);
  rpc_outcome(c, register_req(SRC_B, BOOT_1, EPOCH_1), o, &err);
  rpc_outcome(c, publish_node_req(DependencyNodeId(2), NodeKind::ARTIFACT, "artifact", SRC_B, BOOT_1, EPOCH_1), o, &err);
  rpc_outcome(c, publish_gen_req(DependencyNodeId(2), DependencyNodeGeneration(1), SRC_B, BOOT_1, EPOCH_1), o, &err);
  rpc_outcome(c, add_edge_req(make_edge(DependencyEdgeId(1), DependencyNodeId(1), DependencyNodeId(2), DependencyNodeGeneration(1)), SRC_B, BOOT_1, EPOCH_1), o, &err);
  rpc(c, bye(), resp, &err);
  c.close();
  write_file(marker_file(markerDir, "b_published.flag"), "B1");
  return 0;
}

int run_consumer(uint16_t port, const std::string& markerDir) {
  CoordinatorClient c; std::string err;
  if (!c.open("127.0.0.1", port, &err)) return 1;
  proto::Frame resp; Outcome o;
  rpc(c, hello_req(), resp, &err);
  rpc_outcome(c, register_req(SRC_C, BOOT_1, EPOCH_1), o, &err);
  rpc_outcome(c, publish_node_req(DependencyNodeId(3), NodeKind::WORKLOAD_PHASE, "phase", SRC_C, BOOT_1, EPOCH_1), o, &err);
  rpc_outcome(c, add_edge_req(make_edge(DependencyEdgeId(2), DependencyNodeId(2), DependencyNodeId(3), DependencyNodeGeneration(1)), SRC_C, BOOT_1, EPOCH_1), o, &err);
  ReadinessState st; DependencyNodeGeneration gen; std::vector<BlockingDependency> bl;
  proto::Frame q;
  rpc(c, query_req(DependencyNodeId(3)), q, &err);
  bool ok = parse_query_resp(q, st, gen, bl);
  write_file(marker_file(markerDir, "consumer_result.txt"), ok ? std::string(name_of(st)) : "ERR");
  rpc(c, bye(), resp, &err);
  c.close();
  return 0;
}

int run_worker_a2(uint16_t port, const std::string& markerDir) {
  CoordinatorClient c; std::string err;
  if (!c.open("127.0.0.1", port, &err)) return 1;
  proto::Frame resp;
  rpc(c, hello_req(), resp, &err);
  Outcome reg, pub;
  rpc_outcome(c, register_req(SRC_A, BOOT_2, EPOCH_2), reg, &err);
  rpc_outcome(c, publish_gen_req(DependencyNodeId(1), DependencyNodeGeneration(2), SRC_A, BOOT_2, EPOCH_2), pub, &err);
  ReadinessState mst; DependencyNodeGeneration mgen; std::vector<BlockingDependency> mbl;
  proto::Frame q; rpc(c, query_req(DependencyNodeId(1)), q, &err);
  bool qok = parse_query_resp(q, mst, mgen, mbl);
  rpc(c, bye(), resp, &err);
  c.close();
  write_file(marker_file(markerDir, "a2_published.flag"),
             std::string("reg=") + std::string(name_of(reg)) + ":pub=" + std::string(name_of(pub)) +
             ":qgen=" + (qok ? std::to_string(mgen.value()) : "ERR"));
  return 0;
}

int run_replay(uint16_t port, const std::string& markerDir) {
  CoordinatorClient c; std::string err;
  if (!c.open("127.0.0.1", port, &err)) return 1;
  proto::Frame resp; Outcome o;
  rpc(c, hello_req(), resp, &err);
  rpc_outcome(c, publish_gen_req(DependencyNodeId(1), DependencyNodeGeneration(999), SRC_A, BOOT_1, EPOCH_1), o, &err);
  write_file(marker_file(markerDir, "replay_outcome.txt"), std::string(name_of(o)));
  rpc(c, bye(), resp, &err);
  c.close();
  return 0;
}

#ifdef _WIN32
HANDLE spawn(const std::string& exe, const std::string& args) {
  const std::wstring q(1, L'"');
  std::wstring cmd = q + std::wstring(exe.begin(), exe.end()) + q + L" " + std::wstring(args.begin(), args.end());
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  si.hStdInput = nul; si.hStdOutput = nul; si.hStdError = nul;
  PROCESS_INFORMATION pi{};
  bool ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!ok) {
    proof_log("CreateProcessW failed");
    return nullptr;
  }
  CloseHandle(pi.hThread);
  return pi.hProcess;
}

bool wait_process(HANDLE h, int timeout_ms) {
  return WaitForSingleObject(h, static_cast<DWORD>(timeout_ms)) == WAIT_OBJECT_0;
}
void kill_process(HANDLE h) { TerminateProcess(h, 1); }

inline std::string widen_to_narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  if (len <= 0) return {};
  std::string r(static_cast<std::size_t>(len), 0);
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &r[0], len, nullptr, nullptr);
  return r;
}

std::string abs_path(const std::string& p) {
  std::wstring wp(p.begin(), p.end());
  CreateDirectoryW(wp.c_str(), nullptr);
  wchar_t full[32768];
  DWORD fl = GetFullPathNameW(wp.c_str(), 32768, full, nullptr);
  if (fl > 0 && fl < 32768) return widen_to_narrow(std::wstring(full, full + fl));
  return p;
}

int run_orchestrator(uint16_t port, std::string dir, const std::string& exe) {
  if (port == 0) port = proto::find_free_port();
  dir = abs_path(dir);
  const std::string portStr = std::to_string(port);
  // Clean stale markers from prior runs so wait_for never matches an old file.
  for (const char* m : {"coord.ready","a_published.flag","b_published.flag","consumer_result.txt",
                        "a2_published.flag","replay_outcome.txt","graph.bin","proof_result.txt"}) {
    std::remove(marker_file(dir, m).c_str());
  }
  PROOF_STEP("orchestrator start, port=" + portStr + " dir=" + dir);
  const std::string ready = marker_file(dir, "coord.ready");
  HANDLE coord = nullptr;

  // 1. Start coordinator.
  {
    std::string a = "--role coordinator --port " + portStr + " --ready " + qstr(ready);
    PROOF_STEP(std::string("spawn coordinator") + " args=" + a);
    coord = spawn(exe, a);
    if (!coord) { write_file(marker_file(dir, "proof_result.txt"), "SPAWN_COORDINATOR_FAIL"); return 1; }
    wait_for(ready, 20000);
    if (!file_exists(ready)) { write_file(marker_file(dir, "proof_result.txt"), "COORD_NOT_READY"); return 1; }
    PROOF_STEP("coordinator ready");
  }

  // 2. Worker A publishes MODEL gen1 (process stays alive).
  HANDLE wa = spawn(exe, "--role worker-a --port " + portStr + " --marker " + qstr(dir));
  wait_for(marker_file(dir, "a_published.flag"), 15000);
  PROOF_STEP("worker-a published");

  // 3. Worker B publishes ARTIFACT gen1 + edge.
  HANDLE wb = spawn(exe, "--role worker-b --port " + portStr + " --marker " + qstr(dir));
  (void)wb;
  wait_for(marker_file(dir, "b_published.flag"), 15000);
  PROOF_STEP("worker-b published");

  // 4. Consumer declares phase, queries -> READY.
  HANDLE wc = spawn(exe, "--role consumer --port " + portStr + " --marker " + qstr(dir));
  (void)wc;
  wait_for(marker_file(dir, "consumer_result.txt"), 15000);
  const std::string consumer1 = read_file(marker_file(dir, "consumer_result.txt"));
  if (consumer1 != "READY") { write_file(marker_file(dir, "proof_result.txt"), "STEP4_CONSUMER_NOT_READY:" + consumer1); return 1; }
  PROOF_STEP("consumer1 READY");

  // 5. Kill Worker A as a real OS process.
  kill_process(wa);
  wait_process(wa, 5000);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // 6. Advance coordinator authority epoch 1 -> 2.
  {
    CoordinatorClient c; std::string err;
    if (!c.open("127.0.0.1", port, &err)) { write_file(marker_file(dir, "proof_result.txt"), "CLIENT_OPEN_FAIL"); return 1; }
    proto::Frame resp; Outcome o;
    rpc(c, hello_req(), resp, &err);
    rpc_outcome(c, advance_epoch_req(EPOCH_2), o, &err);
    if (o != Outcome::ACCEPTED) { write_file(marker_file(dir, "proof_result.txt"), "ADVANCE_EPOCH_FAIL"); return 1; }
    c.close();
  }
  PROOF_STEP("advanced epoch to 2");

  // 7. Replay stale Worker A message -> rejected.
  std::remove(marker_file(dir, "replay_outcome.txt").c_str());
  HANDLE wrp = spawn(exe, "--role replay --port " + portStr + " --marker " + qstr(dir));
  (void)wrp;
  wait_for(marker_file(dir, "replay_outcome.txt"), 15000);
  const std::string replay = read_file(marker_file(dir, "replay_outcome.txt"));
  if (replay != "REJECT_STALE_EPOCH" && replay != "REJECT_STALE_BOOT") {
    write_file(marker_file(dir, "proof_result.txt"), "STEP7_STALE_NOT_REJECTED:" + replay);
    return 1;
  }
  PROOF_STEP("stale replay rejected: " + replay);

  // 8. Worker A' (fresh boot, epoch 2) publishes MODEL gen2.
  HANDLE wa2 = spawn(exe, "--role worker-a2 --port " + portStr + " --marker " + qstr(dir));
  (void)wa2;
  wait_for(marker_file(dir, "a2_published.flag"), 15000);
  PROOF_STEP("worker-a2 published model gen2");

  // 9. Query: Artifact(2) STALE, Consumer phase(3) BLOCKED.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  {
    CoordinatorClient c; std::string err;
    if (!c.open("127.0.0.1", port, &err)) return 1;
    proto::Frame resp; Outcome o;
    rpc(c, hello_req(), resp, &err);
    ReadinessState st, st3; DependencyNodeGeneration gen, gen3; std::vector<BlockingDependency> bl, bl3;
    rpc(c, query_req(DependencyNodeId(2)), resp, &err); parse_query_resp(resp, st, gen, bl);
    rpc(c, query_req(DependencyNodeId(3)), resp, &err); parse_query_resp(resp, st3, gen3, bl3);
    DependencyNodeGeneration mgen; ReadinessState mst; std::vector<BlockingDependency> mbl;
    rpc(c, query_req(DependencyNodeId(1)), resp, &err); parse_query_resp(resp, mst, mgen, mbl);
    if (st != ReadinessState::STALE || st3 != ReadinessState::BLOCKED) {
      write_file(marker_file(dir, "proof_result.txt"),
        "STEP9_FAIL:" + std::string(name_of(st)) + ":" + std::string(name_of(st3)) +
        ":modelgen=" + std::to_string(mgen.value()) + ":modelstate=" + std::string(name_of(mst)));
      return 1;
    }
    PROOF_STEP("after gen2: artifact=STALE phase=BLOCKED");

    // 10. Controller rebuilds Artifact against gen2 and rebinds phase.
    rpc_outcome(c, register_req(SRC_ORCH, BOOT_1, EPOCH_2), o, &err);
    rpc_outcome(c, remove_edge_req(DependencyEdgeId(1), SRC_ORCH, BOOT_1, EPOCH_2), o, &err);
    rpc_outcome(c, add_edge_req(make_edge(DependencyEdgeId(5), DependencyNodeId(1), DependencyNodeId(2), DependencyNodeGeneration(2)), SRC_ORCH, BOOT_1, EPOCH_2), o, &err);
    rpc_outcome(c, publish_gen_req(DependencyNodeId(2), DependencyNodeGeneration(2), SRC_ORCH, BOOT_1, EPOCH_2), o, &err);
    rpc_outcome(c, remove_edge_req(DependencyEdgeId(2), SRC_ORCH, BOOT_1, EPOCH_2), o, &err);
    rpc_outcome(c, add_edge_req(make_edge(DependencyEdgeId(6), DependencyNodeId(2), DependencyNodeId(3), DependencyNodeGeneration(2)), SRC_ORCH, BOOT_1, EPOCH_2), o, &err);
    ReadinessState stc; DependencyNodeGeneration genC; std::vector<BlockingDependency> blC;
    rpc(c, query_req(DependencyNodeId(3)), resp, &err); parse_query_resp(resp, stc, genC, blC);
    if (stc != ReadinessState::READY) { write_file(marker_file(dir, "proof_result.txt"), "STEP10_NOT_READY:" + std::string(name_of(stc))); return 1; }
    PROOF_STEP("rebuilt: phase READY");

    // 11. Persist.
    const std::string statePath = marker_file(dir, "graph.bin");
    rpc(c, persist_req(statePath), resp, &err);
    Outcome po; std::uint64_t digest;
    parse_persist_resp(resp, po, digest);
    if (po != Outcome::ACCEPTED) { write_file(marker_file(dir, "proof_result.txt"), "STEP11_PERSIST_FAIL"); return 1; }
    PROOF_STEP("persisted, digest=" + std::to_string(digest));
    c.close();

    // 12. Kill coordinator, restart with --load.
    kill_process(coord);
    wait_process(coord, 5000);
    std::remove(ready.c_str());
    HANDLE coord2 = spawn(exe, "--role coordinator --port " + portStr + " --ready " + qstr(ready) + " --load " + qstr(statePath));
    wait_for(ready, 20000);
    if (!file_exists(ready)) { write_file(marker_file(dir, "proof_result.txt"), "STEP12_RESTART_NOT_READY"); return 1; }
    PROOF_STEP("coordinator restarted from persisted state");

    // 13. Recovered dynamic readiness is conservative: REVALIDATION_REQUIRED.
    if (!c.open("127.0.0.1", port, &err)) { write_file(marker_file(dir, "proof_result.txt"), "STEP13_OPEN_FAIL"); return 1; }
    rpc(c, hello_req(), resp, &err);
    ReadinessState r2; DependencyNodeGeneration g2; std::vector<BlockingDependency> b2;
    rpc(c, query_req(DependencyNodeId(3)), resp, &err); parse_query_resp(resp, r2, g2, b2);
    if (r2 != ReadinessState::REVALIDATION_REQUIRED) {
      write_file(marker_file(dir, "proof_result.txt"), "STEP13_NOT_CONSERVATIVE:" + std::string(name_of(r2)));
      return 1;
    }
    PROOF_STEP("after restart: conservative REVALIDATION_REQUIRED");

    // 14. Republish current evidence bottom-up -> READY.
    rpc_outcome(c, register_req(SRC_ORCH, BOOT_1, EPOCH_2), o, &err);
    rpc_outcome(c, publish_readiness_req(DependencyNodeId(1), SRC_ORCH, BOOT_1, EPOCH_2), o, &err);
    rpc_outcome(c, publish_readiness_req(DependencyNodeId(2), SRC_ORCH, BOOT_1, EPOCH_2), o, &err);
    rpc_outcome(c, publish_readiness_req(DependencyNodeId(3), SRC_ORCH, BOOT_1, EPOCH_2), o, &err);
    ReadinessState stv; DependencyNodeGeneration gv; std::vector<BlockingDependency> bv;
    rpc(c, query_req(DependencyNodeId(3)), resp, &err); parse_query_resp(resp, stv, gv, bv);
    if (stv != ReadinessState::READY) { write_file(marker_file(dir, "proof_result.txt"), "STEP14_NOT_READY:" + std::string(name_of(stv))); return 1; }
    PROOF_STEP("revalidated: phase READY");
    c.close();
    kill_process(coord2);
    wait_process(coord2, 4000);
  }

  write_file(marker_file(dir, "proof_result.txt"), "PASS");
  PROOF_STEP("PROOF PASS");
  return 0;
}
#else
int run_orchestrator(uint16_t, std::string, const std::string&) {
  write_file("proof_result.txt", "ORCHESTRATOR_REQUIRES_WINDOWS");
  return 1;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  std::string role = "orchestrator";
  std::uint16_t port = 0;
  std::string dir = ".";
  std::string exe = "";
  std::string ready = "coord.ready";
  std::string load = "";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--role" && i + 1 < argc) role = argv[++i];
    else if (a == "--port" && i + 1 < argc) port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
    else if (a == "--marker" && i + 1 < argc) dir = argv[++i];
    else if (a == "--dir" && i + 1 < argc) dir = argv[++i];
    else if (a == "--exe" && i + 1 < argc) exe = argv[++i];
    else if (a == "--ready" && i + 1 < argc) ready = argv[++i];
    else if (a == "--load" && i + 1 < argc) load = argv[++i];
  }
  if (role == "coordinator") return run_coordinator(port, ready, load);
  if (role == "worker-a") return run_worker_a(port, dir);
  if (role == "worker-b") return run_worker_b(port, dir);
  if (role == "consumer") return run_consumer(port, dir);
  if (role == "worker-a2") return run_worker_a2(port, dir);
  if (role == "replay") return run_replay(port, dir);
  if (role == "orchestrator") {
    if (exe.empty()) {
#ifdef _WIN32
      std::wstring wbuf(32768, L'\0');
      DWORD n = GetModuleFileNameW(nullptr, wbuf.data(), static_cast<DWORD>(wbuf.size()));
      if (n > 0) exe = widen_to_narrow(std::wstring(wbuf.data(), n));
      else exe = argv[0];
#else
      exe = argv[0];
#endif
    }
    return run_orchestrator(port, dir, exe);
  }
  return 1;
}