#pragma once

// VENDORED from tick2trade (github.com/Other-Now/tick2trade, include/t2t/ouch.hpp).
// Changes from the original, all mechanical:
//   * __builtin_bswap* replaced by shift-based stores so it builds on MSVC too;
//   * time-in-force is a field (was hard-coded to 99999 = day) so IOC flatten
//     orders can go on the wire;
//   * load_be* helpers added for the simulated exchange's decoder.
//
// NASDAQ OUCH 4.2 EnterOrder, 49 bytes, big-endian, written field by field into
// a caller-supplied buffer: no allocation, no serialisation library.
//
// OUCH is a stand-in wire format. Indian exchanges use their own binary
// protocols (e.g. NSE NNF), which carry the exchange-issued algo id; OUCH has no
// such field, so here the algo id stays inside the gate and the audit log.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace arg::ouch {

constexpr std::size_t kEnterOrderLen = 49;

inline void store_be32(unsigned char* p, std::uint32_t v) {
    p[0] = static_cast<unsigned char>(v >> 24); p[1] = static_cast<unsigned char>(v >> 16);
    p[2] = static_cast<unsigned char>(v >> 8);  p[3] = static_cast<unsigned char>(v);
}
inline void store_be64(unsigned char* p, std::uint64_t v) {
    store_be32(p, static_cast<std::uint32_t>(v >> 32)); store_be32(p + 4, static_cast<std::uint32_t>(v));
}
inline std::uint32_t load_be32(const unsigned char* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
inline std::uint64_t load_be64(const unsigned char* p) {
    return (std::uint64_t(load_be32(p)) << 32) | load_be32(p + 4);
}

struct EnterOrder {
    std::uint64_t token;    // client order token (14 bytes on the wire: 6 spaces + 8 BE bytes)
    std::uint64_t symbol;   // 8 ASCII bytes, already packed
    std::uint32_t shares;
    std::uint32_t price;
    bool          buy;
    std::uint32_t tif;      // 0 = IOC, 99999 = day
};

// Returns bytes written. `dst` must have room for kEnterOrderLen.
inline std::size_t encode(unsigned char* dst, const EnterOrder& o) noexcept {
    unsigned char* p = dst;
    *p++ = 'O';                                  //  0: message type
    std::memset(p, ' ', 6); p += 6;              //  1: order token (padding)
    store_be64(p, o.token); p += 8;              //  7: order token (numeric)
    *p++ = o.buy ? 'B' : 'S';                    // 15: buy/sell indicator
    store_be32(p, o.shares); p += 4;             // 16: shares
    store_be64(p, o.symbol); p += 8;             // 20: stock symbol (8 bytes)
    store_be32(p, o.price); p += 4;              // 28: price
    store_be32(p, o.tif);   p += 4;              // 32: time in force
    store_be32(p, 0);       p += 4;              // 36: firm (0 = default)
    *p++ = 'P';                                  // 40: display
    *p++ = 'Y';                                  // 41: capacity
    *p++ = 'N';                                  // 42: intermarket sweep eligibility
    store_be32(p, 0); p += 4;                    // 43: minimum quantity
    *p++ = 'N';                                  // 47: cross type
    *p++ = 'N';                                  // 48: customer type
    return static_cast<std::size_t>(p - dst);
}

}  // namespace arg::ouch
