// dependency_fabric::test — minimal header-only test harness.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace df_test {

inline int& failures() { static int f = 0; return f; }
inline int& checks() { static int c = 0; return c; }

inline void fail(const char* file, int line, const std::string& msg) {
  ++failures();
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, msg.c_str());
}

inline int summary(const char* suite) {
  std::printf("[%s] %d checks, %d failures\n", suite, checks(), failures());
  return failures() == 0 ? 0 : 1;
}

}  // namespace df_test

#define CHECK(cond)                                                              do {                                                                             ++df_test::checks();                                                           if (!(cond)) df_test::fail(__FILE__, __LINE__, #cond);                       } while (0)

#define CHECK_EQ(a, b)                                                           do {                                                                             ++df_test::checks();                                                           auto _va = (a);                                                                auto _vb = (b);                                                                if (!(_va == _vb)) {                                                             df_test::fail(__FILE__, __LINE__,                                         \
                    std::string("CHECK_EQ(") + #a + ", " + #b + ")");           \
    }                                                                          \
  } while (0)
