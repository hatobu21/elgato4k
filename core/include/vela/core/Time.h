#pragma once
#include <cstdint>

namespace vela::core {

// All host-clock values in the core are microseconds on one monotonic clock
// (QueryPerformanceCounter on Windows, steady_clock in tests). GPU timestamp
// durations are converted to milliseconds before they reach the core and are
// never subtracted from host times.
using Micros = int64_t;

constexpr double toMs(Micros us) { return static_cast<double>(us) / 1000.0; }
constexpr Micros fromMs(double ms) { return static_cast<Micros>(ms * 1000.0 + (ms >= 0 ? 0.5 : -0.5)); }

} // namespace vela::core
