#pragma once
// Minimal test harness: no third-party dependency, so the core builds and
// tests the same way under MSVC, GCC and Clang.
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace vela::test {
struct Case { const char* name; std::function<void()> body; };
inline std::vector<Case>& registry() { static std::vector<Case> cases; return cases; }
inline int& failures() { static int count = 0; return count; }
struct Register { Register(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); } };
} // namespace vela::test

#define VELA_CAT2(a, b) a##b
#define VELA_CAT(a, b) VELA_CAT2(a, b)
#define TEST(name) \
    static void name(); \
    static ::vela::test::Register VELA_CAT(reg_, name)(#name, name); \
    static void name()

#define CHECK(cond) do { if (!(cond)) { ++::vela::test::failures(); \
    std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, tol) do { const double va_ = (a), vb_ = (b); if (std::fabs(va_ - vb_) > (tol)) { \
    ++::vela::test::failures(); std::printf("  FAIL %s:%d  %s = %.4f, expected %.4f +- %.4f\n", \
    __FILE__, __LINE__, #a, va_, vb_, double(tol)); } } while (0)
