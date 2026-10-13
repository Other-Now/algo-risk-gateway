#pragma once

// TSC timestamps for the benchmark only -- the gate itself never reads a clock.
//
// Plain rdtsc, not rdtscp: tick2trade measured the serialising rdtscp inflating
// a ~40 ns pipeline by 14-55% on x86 by destroying overlap between consecutive
// events. Per-check numbers still carry some probe effect; the README reports
// the un-probed whole-gate number next to them so the gap is visible.

#include <algorithm>
#include <chrono>
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace arg {

inline std::uint64_t ticks() noexcept {
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// ns per tick, measured against steady_clock over ~50 ms.
inline double calibrate_ns_per_tick() {
    using clk = std::chrono::steady_clock;
    const auto w0 = clk::now();
    const std::uint64_t t0 = ticks();
    while (clk::now() - w0 < std::chrono::milliseconds(50)) {}
    const auto w1 = clk::now();
    const std::uint64_t t1 = ticks();
    const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0).count());
    return ns / double(t1 - t0);
}

// Median cost of an empty ticks()-ticks() interval; subtracted from samples.
inline std::uint64_t probe_overhead_ticks() {
    std::uint64_t best[1001];
    for (int i = 0; i < 1001; ++i) { const auto a = ticks(); const auto b = ticks(); best[i] = b - a; }
    std::uint64_t* mid = best + 500;
    std::nth_element(best, mid, best + 1001);
    return *mid;
}

}  // namespace arg
