#pragma once

// VENDORED from tick2trade (include/t2t/histogram.hpp); only change is
// std::countl_zero in place of __builtin_clzll so it builds on MSVC.
//
// An HdrHistogram-style recorder: constant-time record, bounded memory, and
// percentiles that stay accurate out to p99.999.
//
// Why not "collect a vector and sort it": at 10M+ events that is hundreds of MB
// and the allocation itself perturbs what we are measuring. Why not a running
// mean/stddev: latency distributions are not normal -- they are multi-modal with
// a long tail, and the mean sits in a valley between the modes. The tail is the
// product; the mean is noise.
//
// Layout: values are bucketed by (magnitude, sub-bucket) so precision is
// *relative* -- kSigFigs significant digits at every magnitude. 1050 ns lands in
// its own bin, and so does 1,050,000 ns, at a fixed memory cost.

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace arg {

class Histogram {
public:
    static constexpr int kSigFigs = 3;                 // ~0.1% relative precision
    static constexpr int kSubBucketCount = 2048;       // 2^11 >= 10^3 * 2
    static constexpr int kSubBucketMask = kSubBucketCount - 1;
    static constexpr int kSubBucketHalf = kSubBucketCount / 2;
    static constexpr int kBucketCount = 40;            // covers up to ~2^50 ns

    Histogram() : counts_(static_cast<std::size_t>(kBucketCount) * kSubBucketHalf + kSubBucketHalf, 0) {}

    inline void record(std::uint64_t v) noexcept {
        const std::size_t idx = index_for(v);
        if (idx < counts_.size()) {
            ++counts_[idx];
            ++total_;
            if (v > max_) max_ = v;
            if (v < min_) min_ = v;
            sum_ += v;
        }
    }

    std::uint64_t count() const { return total_; }
    std::uint64_t max() const { return total_ ? max_ : 0; }
    std::uint64_t min() const { return total_ ? min_ : 0; }
    double mean() const { return total_ ? double(sum_) / double(total_) : 0.0; }

    // Value at or below which `p` fraction of samples fall.
    std::uint64_t percentile(double p) const {
        if (!total_) return 0;
        const std::uint64_t target =
            static_cast<std::uint64_t>(p / 100.0 * double(total_) + 0.5);
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= target && counts_[i]) return value_at(i);
        }
        return max_;
    }

    void merge(const Histogram& o) {
        for (std::size_t i = 0; i < counts_.size(); ++i) counts_[i] += o.counts_[i];
        total_ += o.total_;
        sum_ += o.sum_;
        max_ = std::max(max_, o.max_);
        if (o.total_) min_ = std::min(min_, o.min_);
    }

    void reset() {
        std::fill(counts_.begin(), counts_.end(), 0);
        total_ = 0; sum_ = 0; max_ = 0; min_ = UINT64_MAX;
    }

    // One-line summary row. `scale` converts stored units to the display unit.
    std::string row(const char* label, double scale = 1.0) const {
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "%-22s %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %12.1f",
                      label,
                      double(percentile(50)) * scale,
                      double(percentile(90)) * scale,
                      double(percentile(99)) * scale,
                      double(percentile(99.9)) * scale,
                      double(percentile(99.99)) * scale,
                      double(percentile(99.999)) * scale,
                      double(max()) * scale);
        return buf;
    }

    static std::string header() {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%-22s %10s %10s %10s %10s %10s %10s %12s",
                      "", "p50", "p90", "p99", "p99.9", "p99.99", "p99.999", "max");
        return buf;
    }

    // Log-scaled bar chart of the distribution -- the shape matters as much as
    // the percentiles (a bimodal p99 means something different from a smooth one).
    std::string histogram_bars(double scale, const char* unit) const {
        if (!total_) return "(no samples)\n";
        struct Row { double lo, hi; std::uint64_t n; };
        std::vector<Row> rows;
        const double lo = double(min_) * scale, hi = double(max_) * scale;
        const int kRows = 14;
        const double step = (hi - lo) / kRows;
        for (int i = 0; i < kRows; ++i) rows.push_back({lo + i * step, lo + (i + 1) * step, 0});
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            if (!counts_[i]) continue;
            const double v = double(value_at(i)) * scale;
            int b = (step > 0) ? int((v - lo) / step) : 0;
            b = std::max(0, std::min(kRows - 1, b));
            rows[static_cast<std::size_t>(b)].n += counts_[i];
        }
        std::uint64_t peak = 0;
        for (const auto& r : rows) peak = std::max(peak, r.n);
        std::string out;
        char buf[256];
        for (const auto& r : rows) {
            const int bar = peak ? int(40.0 * double(r.n) / double(peak)) : 0;
            std::snprintf(buf, sizeof buf, "  %8.0f-%8.0f %s |%-40s %llu\n", r.lo, r.hi, unit,
                          std::string(static_cast<std::size_t>(bar), '#').c_str(),
                          static_cast<unsigned long long>(r.n));
            out += buf;
        }
        return out;
    }

private:
    static std::size_t index_for(std::uint64_t v) {
        if (v < kSubBucketCount) return static_cast<std::size_t>(v);
        const int msb = 63 - std::countl_zero(v);
        const int bucket = msb - 10;  // log2(kSubBucketHalf)
        const std::uint64_t sub = v >> bucket;
        return static_cast<std::size_t>(bucket) * kSubBucketHalf +
               static_cast<std::size_t>(sub);
    }

    // Inverse of index_for. For idx >= kSubBucketCount the encoding is
    // idx = bucket*half + sub with sub in [half, 2*half), so idx lands in
    // [(bucket+1)*half, (bucket+2)*half) and the bucket is idx/half - 1.
    static std::uint64_t value_at(std::size_t idx) {
        if (idx < kSubBucketCount) return idx;
        const std::size_t bucket = idx / kSubBucketHalf - 1;
        const std::size_t sub = idx - bucket * kSubBucketHalf;
        return static_cast<std::uint64_t>(sub) << bucket;
    }

    std::vector<std::uint64_t> counts_;
    std::uint64_t total_{0}, sum_{0}, max_{0}, min_{UINT64_MAX};
};

}  // namespace arg
