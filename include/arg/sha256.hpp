#pragma once

// Minimal SHA-256 (FIPS 180-4). Used only to chain audit records; it is never
// on the order path. Checked against the FIPS test vectors in arg_tests.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace arg {

using Digest = std::array<std::uint8_t, 32>;

class Sha256 {
public:
    Sha256() { reset(); }

    void reset() {
        h_[0] = 0x6a09e667; h_[1] = 0xbb67ae85; h_[2] = 0x3c6ef372; h_[3] = 0xa54ff53a;
        h_[4] = 0x510e527f; h_[5] = 0x9b05688c; h_[6] = 0x1f83d9ab; h_[7] = 0x5be0cd19;
        len_ = 0;
        buf_len_ = 0;
    }

    void update(const void* data, std::size_t n) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        len_ += n;
        while (n > 0) {
            const std::size_t take = (64 - buf_len_ < n) ? 64 - buf_len_ : n;
            std::memcpy(buf_ + buf_len_, p, take);
            buf_len_ += take; p += take; n -= take;
            if (buf_len_ == 64) { block(buf_); buf_len_ = 0; }
        }
    }

    Digest finish() {
        const std::uint64_t bits = len_ * 8;
        const std::uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (buf_len_ != 56) update(&zero, 1);
        std::uint8_t lenbe[8];
        for (int i = 0; i < 8; ++i) lenbe[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        update(lenbe, 8);
        Digest d;
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j) d[4 * i + j] = static_cast<std::uint8_t>(h_[i] >> (24 - 8 * j));
        return d;
    }

    static Digest of(const void* data, std::size_t n) {
        Sha256 s;
        s.update(data, n);
        return s.finish();
    }

private:
    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const std::uint8_t* b) {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(b[4 * i]) << 24) | (std::uint32_t(b[4 * i + 1]) << 16) |
                   (std::uint32_t(b[4 * i + 2]) << 8) | std::uint32_t(b[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + S1 + ch + k[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t mj = (a & bb) ^ (a & c) ^ (bb & c);
            const std::uint32_t t2 = S0 + mj;
            h = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += bb; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }

    std::uint32_t h_[8];
    std::uint64_t len_;
    std::uint8_t buf_[64];
    std::size_t buf_len_;
};

}  // namespace arg
