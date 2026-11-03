#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ostream>
namespace rrl {
struct LatencySummary {
    std::uint64_t count{0},total_ns{0},p50_ns{0},p95_ns{0},p99_ns{0},p999_ns{0},max_ns{0};
};
// Fixed storage. Exact through 127 ns; then 64 subdivisions per power of two.
// Quantiles return bucket upper bounds (relative error < 1/64), never samples
// collected by allocation while a transaction holds locks.
class LatencyHistogram {
public:
    void add(std::uint64_t ns) {
        unsigned index=static_cast<unsigned>(ns);
        if(ns>=64) {
            unsigned e=0;for(auto v=ns;v>1;v>>=1)++e;
            index=(e-5)*64+static_cast<unsigned>((ns>>(e-6))-64);
        }
        ++bins_[index];++count_;total_+=ns;max_=std::max(max_,ns);
    }
    void merge(const LatencyHistogram& o) {
        for(unsigned i=0;i<bins_.size();++i)bins_[i]+=o.bins_[i];
        count_+=o.count_;total_+=o.total_;max_=std::max(max_,o.max_);
    }
    LatencySummary summary() const {
        return {count_,total_,quantile(0.5L),quantile(0.95L),quantile(0.99L),quantile(0.999L),max_};
    }
    void write_json(std::ostream& out) const {
        out << "{\"count\":" << count_ << ",\"max_ns\":" << max_ << ",\"total_ns\":" << total_ << ",\"relative_error_bound\":0.015625,\"buckets\":[";
        bool first=true;
        for(unsigned i=0;i<bins_.size();++i) {
            if(!bins_[i])continue;
            const auto shift=i<64 ? 0 : i/64-1;
            const auto hi=static_cast<std::uint64_t>(65+i%64);
            const auto lower=i<64 ? i : (static_cast<std::uint64_t>(64+i%64)<<shift);
            const auto upper=i<64 ? i : shift==57 && hi==128 ? std::numeric_limits<std::uint64_t>::max() : (hi<<shift)-1;
            if(!first)out << ',';
            first=false;
            out << "{\"lower_ns\":" << lower << ",\"upper_ns\":" << upper << ",\"count\":" << bins_[i] << '}';
        }
        out << "]}";
    }
private:
    std::uint64_t quantile(long double p) const {
        if(!count_)return 0;
        const auto rank=static_cast<std::uint64_t>(std::ceil(p*count_));
        std::uint64_t seen=0;
        for(unsigned i=0;i<bins_.size();++i) {
            seen+=bins_[i];if(seen<rank)continue;
            if(i<64)return i;
            const auto shift=i/64-1;
            const auto hi=static_cast<std::uint64_t>(65+i%64);
            if(shift==57 && hi==128)return max_;
            return std::min(max_,(hi<<shift)-1);
        }
        return max_;
    }
    std::array<std::uint64_t,3776> bins_{};
    std::uint64_t count_{0},total_{0},max_{0};
};
} // namespace rrl
