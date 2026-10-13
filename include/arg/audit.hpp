#pragma once

// Append-only, hash-chained audit trail.
//
// Every *input* the gate sees (orders, cancel requests, fills, done reports,
// market ticks, timer ticks, kill commands) and every *output* it produces
// (the decision + reason code on each order, kill triggers, risk-initiated
// cancels, flatten orders) is one fixed 64-byte Record.
//
// File layout:
//   "ARGAUD01" | u32 config_size | config bytes | (Record, 32-byte digest)*
//   digest_0 = SHA256(magic || config bytes)
//   digest_i = SHA256(digest_{i-1} || record_i)
//
// Two layers of protection, and they catch different things:
//   1. The chain. Flip any byte of any record (or the config) and every digest
//      from that point on is wrong. This catches corruption and naive edits.
//   2. Replay. The gate is a deterministic function of its inputs, so
//      re-running the logged inputs must reproduce every logged output
//      byte-for-byte. An attacker who edits a decision AND recomputes the
//      chain still fails here. (An attacker who rewrites inputs and outputs
//      consistently is caught only by anchoring the head digest somewhere they
//      can't write -- see README, "What this is not".)
//
// Appending is a 64-byte copy into a pre-reserved vector; hashing happens when
// the log is sealed to disk, off the decision path.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "arg/config.hpp"
#include "arg/sha256.hpp"
#include "arg/types.hpp"

namespace arg {

enum class Rec : std::uint8_t {
    // inputs
    Order = 1,       // NewOrder in; `reason` is the decision (output)
    CancelReq = 2,   // algo asked to cancel id
    FillIn = 3,      // exchange fill; aux = exec_id
    Done = 4,        // exchange: order no longer working (cancelled / IOC expired)
    Market = 5,      // last traded price for instr
    Timer = 6,       // clock tick (heartbeat check)
    KillCmd = 7,     // manual kill/reset; flags bit0 = flatten, bit1 = reset
    // outputs
    KillOut = 20,    // a kill fired; reason byte = KillWhy
    CancelOut = 21,  // risk-initiated cancel of id
    FlattenOut = 22, // risk-generated flatten order
};

inline bool is_input(Rec k) { return static_cast<std::uint8_t>(k) < 20; }

#pragma pack(push, 1)
struct Record {
    std::uint64_t seq;
    std::uint64_t ts;
    std::uint64_t id;
    std::int64_t price;
    std::int64_t qty;
    std::uint64_t aux;
    std::uint32_t algo;
    std::uint16_t instr;
    std::uint8_t kind;
    std::uint8_t side;
    std::uint8_t tif;
    std::uint8_t reason;
    std::uint8_t scope;
    std::uint8_t flags;
    std::uint8_t pad[4];
};
#pragma pack(pop)
static_assert(sizeof(Record) == 64, "audit record must stay 64 bytes");

constexpr char kMagic[8] = {'A', 'R', 'G', 'A', 'U', 'D', '0', '1'};

class AuditLog {
public:
    explicit AuditLog(const Config& cfg, std::size_t reserve = 1 << 20) : cfg_(cfg) {
        // reserve() only reserves address space; the first write to each 4 KB page
        // would then take a page fault on the order path (~1 in 64 appends, several
        // microseconds each -- it showed up as the audit p99.9). Touch it all now.
        recs_.resize(reserve);
        recs_.clear();
    }

    Record& append(Rec kind, Ts ts) {
        recs_.emplace_back();
        Record& r = recs_.back();
        std::memset(&r, 0, sizeof r);
        r.seq = recs_.size() - 1;
        r.ts = ts;
        r.kind = static_cast<std::uint8_t>(kind);
        return r;
    }

    const std::vector<Record>& records() const { return recs_; }
    const Config& config() const { return cfg_; }
    void clear() { recs_.clear(); }

    Digest seed() const {
        Sha256 s;
        s.update(kMagic, sizeof kMagic);
        s.update(&cfg_, sizeof cfg_);
        return s.finish();
    }

    static Digest link(const Digest& prev, const Record& r) {
        Sha256 s;
        s.update(prev.data(), prev.size());
        s.update(&r, sizeof r);
        return s.finish();
    }

    // Seal to disk. Returns the head digest.
    Digest write(const std::string& path) const {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        Digest h = seed();
        if (!f) return h;
        const std::uint32_t n = sizeof cfg_;
        std::fwrite(kMagic, 1, sizeof kMagic, f);
        std::fwrite(&n, sizeof n, 1, f);
        std::fwrite(&cfg_, sizeof cfg_, 1, f);
        for (const Record& r : recs_) {
            h = link(h, r);
            std::fwrite(&r, sizeof r, 1, f);
            std::fwrite(h.data(), 1, h.size(), f);
        }
        std::fclose(f);
        return h;
    }

private:
    Config cfg_;
    std::vector<Record> recs_;
};

struct LoadedLog {
    Config cfg{};
    std::vector<Record> recs;
    std::vector<Digest> digests;
    std::string error;          // non-empty: file unreadable / malformed
    std::int64_t first_bad = -1; // first record whose chain digest does not verify
    Digest head{};
};

inline LoadedLog load_log(const std::string& path) {
    LoadedLog out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { out.error = "cannot open " + path; return out; }
    char magic[8];
    std::uint32_t n = 0;
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, kMagic, 8) != 0) {
        out.error = "bad magic"; std::fclose(f); return out;
    }
    if (std::fread(&n, sizeof n, 1, f) != 1 || n != sizeof(Config)) {
        out.error = "config size mismatch (built with different limits layout?)"; std::fclose(f); return out;
    }
    if (std::fread(&out.cfg, sizeof out.cfg, 1, f) != 1) { out.error = "truncated config"; std::fclose(f); return out; }

    Sha256 s;
    s.update(kMagic, sizeof kMagic);
    s.update(&out.cfg, sizeof out.cfg);
    Digest h = s.finish();

    Record r;
    Digest d;
    while (std::fread(&r, sizeof r, 1, f) == 1) {
        if (std::fread(d.data(), 1, d.size(), f) != d.size()) { out.error = "truncated digest"; break; }
        h = AuditLog::link(h, r);
        if (out.first_bad < 0 && h != d) out.first_bad = static_cast<std::int64_t>(out.recs.size());
        out.recs.push_back(r);
        out.digests.push_back(d);
    }
    out.head = h;
    std::fclose(f);
    return out;
}

}  // namespace arg
