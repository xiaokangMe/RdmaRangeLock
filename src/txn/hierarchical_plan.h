#pragma once
#include "txn/workload.h"
#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>

namespace rrl::detail {
struct HierarchyRequest {
    LockMode mode;
    KeyRangeEnt range;
    LockKind kind;
    unsigned phase; // root=0, bucket=1, record=2
};
// Gray et al. (1976), Table 1 and root-to-leaf rules, instantiated as
// root -> fixed partition -> record. Partitions are NOT B+ tree nodes and
// do not implement Graefe's separator-key naming/SMO lock replication.
// The generated trace is known before execution. Select the final mode once,
// before holding any lock. Do not turn a shared read into an in-flight upgrade.
inline std::vector<HierarchyRequest> hierarchy_plan(const TransactionTrace& txn, std::uint64_t width) {
    if (!width) throw std::invalid_argument("hierarchy bucket width must be positive");
    struct Bucket { bool scan{false}, write{false}; LockMode mode{LockMode::IS}; };
    std::map<idx_key_t, Bucket> buckets;
    std::map<idx_key_t, LockMode> records;
    bool writes = false;
    for (const auto& op : txn.operations) {
        if (op.kind == OperationKind::RangeRead) {
            if (op.end_key <= op.key) continue;
            const auto last = (op.end_key - 1) / width;
            for (auto b = op.key / width;; ++b) {
                buckets[b].scan = true;
                if (b == last) break;
            }
        } else {
            const bool write = op.kind != OperationKind::Read;
            buckets[op.key / width].write |= write;
            writes |= write;
            const auto mode = write ? LockMode::X : LockMode::S;
            auto [it, added] = records.emplace(op.key, mode);
            if (!added && write) it->second = LockMode::X;
        }
    }
    std::vector<HierarchyRequest> plan;
    plan.reserve(1 + buckets.size() + records.size());
    plan.push_back({writes ? LockMode::IX : LockMode::IS, KeyRangeEnt::unbounded(), LockKind::Node, 0});
    for (auto& [b, flags] : buckets) {
        // SIX is deliberately not introduced. A scan+write bucket takes X
        // from the outset, conservatively covering its read and write set.
        flags.mode = flags.scan ? (flags.write ? LockMode::X : LockMode::S)
                                : (flags.write ? LockMode::IX : LockMode::IS);
        const idx_key_t base = b * width;
        const auto max = std::numeric_limits<idx_key_t>::max();
        const idx_key_t end = base > max - (width - 1) ? max : base + width - 1;
        plan.push_back({flags.mode, KeyRangeEnt::closed(base, end), LockKind::Node, 1});
    }
    for (const auto& [key, mode] : records) {
        const auto bucket_mode = buckets.at(key / width).mode;
        if (bucket_mode == LockMode::S || bucket_mode == LockMode::X) continue;
        plan.push_back({mode, KeyRangeEnt::point(key), LockKind::Record, 2});
    }
    return plan;
}
} // namespace rrl::detail
