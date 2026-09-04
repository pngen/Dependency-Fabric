// dependency_fabric::test — genuine concurrency (parallel mutation + reads).
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "test_util.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace df_test;
using namespace dependency_fabric;

int main() {
  tf::Fixture f;
  const std::uint64_t n = 64;
  for (std::uint64_t i = 1; i <= n; ++i) {
    CHECK_EQ(f.declare(i, NodeKind::MODEL, ""), Outcome::ACCEPTED);
    CHECK_EQ(f.pub(i, 1), Outcome::ACCEPTED);
  }

  std::atomic<bool> go{false};
  std::atomic<long> failures{0};
  std::vector<std::thread> threads;

  const int kWriters = 6;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&f, &go, &failures, w, n] {
      while (!go.load()) std::this_thread::yield();
      for (int iter = 0; iter < 200; ++iter) {
        const std::uint64_t a = 1 + static_cast<std::uint64_t>((w * 7 + iter * 13) % n);
        const std::uint64_t b = 1 + static_cast<std::uint64_t>((w * 3 + iter * 5) % n);
        if (a == b) continue;
        const std::uint64_t prod = a < b ? a : b;
        const std::uint64_t cons = a < b ? b : a;
        // Monotonic publish on the producer.
        const auto cur = f.g.current_authority(DependencyNodeId(prod));
        const std::uint64_t ngen = cur.value() + 1;
        Outcome o = f.g.publish_generation(DependencyNodeId(prod), DependencyNodeGeneration(ngen), f.src, f.boot, f.epoch);
        if (o != Outcome::ACCEPTED && o != Outcome::REJECT_STALE_NODE_GENERATION && o != Outcome::REJECT_DUPLICATE_ID) ++failures;
        // Add acyclic edges only.
        (void)f.g.add_edge(tf::Fixture::pin_edge(200000 + w * 100000 + iter, prod, cons, 1), f.src, f.boot, f.epoch);
        // Sometimes invalidate (direct) and recover.
        if (iter % 7 == 0) {
          InvalidationPolicy pol; pol.mode = InvalidationMode::DIRECT;
          (void)f.g.invalidate_node(DependencyNodeId(cons), InvalidationReason::EXPLICIT, pol, f.src, f.boot, f.epoch);
          (void)f.g.mark_recovered(DependencyNodeId(cons), f.src, f.boot, f.epoch);
        }
      }
    });
  }

  const int kReaders = 4;
  for (int rd = 0; rd < kReaders; ++rd) {
    threads.emplace_back([&f, &go, &failures, n, rd] {
      while (!go.load()) std::this_thread::yield();
      for (int iter = 0; iter < 2000; ++iter) {
        const std::uint64_t id = 1 + static_cast<std::uint64_t>((rd * 11 + iter * 17) % n);
        (void)f.g.readiness(DependencyNodeId(id));
        (void)f.g.blockers(DependencyNodeId(id));
        (void)f.g.dependents(DependencyNodeId(id), true);
        auto ac = f.g.affected_closure(DependencyNodeId(id), 0);
        (void)ac;
        (void)f.g.recovery_set(DependencyNodeId(id));
        (void)f.g.recompute_frontier(DependencyNodeId(id));
        (void)f.g.recovery_plan(DependencyNodeId(id));
      }
    });
  }

  go.store(true);
  for (auto& t : threads) t.join();

  CHECK_EQ(failures.load(), 0);
  CHECK(f.g.acyclic());
  // Generation monotonicity holds under concurrent publication.
  for (auto nid : f.g.all_nodes()) {
    auto nd = f.g.node(nid);
    if (nd->generation_history.size() >= 2) {
      for (std::size_t i = 1; i < nd->generation_history.size(); ++i) {
        CHECK(nd->generation_history[i].value() > nd->generation_history[i - 1].value());
      }
    }
  }

  return summary("concurrency");
}