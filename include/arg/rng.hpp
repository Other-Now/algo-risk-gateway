#pragma once

// splitmix64. Used instead of <random> distributions because those are not
// specified bit-for-bit across standard libraries -- the sim must produce the
// same session on MSVC and libstdc++.

#include <cstdint>

namespace arg {

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    std::uint64_t below(std::uint64_t n) { return n ? next() % n : 0; }
    std::int64_t range(std::int64_t lo, std::int64_t hi) { return lo + std::int64_t(below(std::uint64_t(hi - lo + 1))); }
    bool chance(std::uint32_t per_10000) { return below(10000) < per_10000; }
};

}  // namespace arg
