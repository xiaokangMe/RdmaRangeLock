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
        // SIX covers the scan with S while retaining IX for actual row X.
        // Other point readers retain IS and can proceed on unrelated rows.
        flags.mode = flags.scan ? (flags.write ? LockMode::SIX : LockMode::S)
                                : (flags.write ? LockMode::IX : LockMode::IS);
        const idx_key_t base = b * width;
        const auto max = std::numeric_limits<idx_key_t>::max();
        const idx_key_t end = base > max - (width - 1) ? max : base + width - 1;
        plan.push_back({flags.mode, KeyRangeEnt::closed(base, end), LockKind::Node, 1});
    }
    for (const auto& [key, mode] : records) {
        const auto bucket_mode = buckets.at(key / width).mode;
        if (bucket_mode == LockMode::X || (mode==LockMode::S &&
            (bucket_mode==LockMode::S || bucket_mode==LockMode::SIX))) continue;
        plan.push_back({mode, KeyRangeEnt::point(key), LockKind::Record, 2});
    }
    return plan;
}

inline bool separator_range_contains(const KeyRangeEnt& outer,const KeyRangeEnt& inner) {
    if (inner.empty()) return true;
    if (outer.empty()) return false;
    if (outer.has_start) {
        if (!inner.has_start || inner.StartKey < outer.StartKey ||
            (inner.StartKey == outer.StartKey &&
             outer.start_bound == Bound::Open && inner.start_bound == Bound::Closed)) return false;
    }
    if (outer.has_end) {
        if (!inner.has_end || inner.EndKey > outer.EndKey ||
            (inner.EndKey == outer.EndKey &&
             outer.end_bound == Bound::Open && inner.end_bound == Bound::Closed)) return false;
    }
    return true;
}

inline bool same_separator_range(const KeyRangeEnt& lhs,const KeyRangeEnt& rhs) {
    return lhs.StartKey==rhs.StartKey && lhs.EndKey==rhs.EndKey &&
           lhs.has_start==rhs.has_start && lhs.has_end==rhs.has_end &&
           lhs.start_bound==rhs.start_bound && lhs.end_bound==rhs.end_bound;
}

// Graefe, "Hierarchical Locking in B-tree Indexes" and TODS 2010 §5.3:
// an interior separator names the half-open range from that separator to the
// next separator.  The index supplies logical cover spans; transaction code
// chooses S/IX/SIX and never locks a physical page id.
inline bool separator_plan(BTreeIndex& index,const TransactionTrace& txn,
                           std::vector<IndexNodeSpan>& scratch,
                           std::vector<HierarchyRequest>& plan) {
    plan.clear();
    auto add=[&](LockMode mode,const KeyRangeEnt& range,LockKind kind,unsigned phase) {
        auto found=std::find_if(plan.begin(),plan.end(),[&](const HierarchyRequest& r) {
            return r.kind==kind && r.phase==phase && same_separator_range(r.range,range);
        });
        if(found!=plan.end()) { found->mode=lock_mode_join(found->mode,mode); return; }
        plan.push_back({mode,range,kind,phase});
    };
    bool writes=false;
    for(const auto& op:txn.operations) {
        if(op.kind!=OperationKind::Read && op.kind!=OperationKind::RangeRead) writes=true;
    }
    add(writes?LockMode::IX:LockMode::IS,KeyRangeEnt::unbounded(),LockKind::Node,0);
    for(const auto& op:txn.operations) {
        const bool scan=op.kind==OperationKind::RangeRead;
        const bool write=!scan && op.kind!=OperationKind::Read;
        const auto query=scan?KeyRangeEnt::half_open(op.key,op.end_key):KeyRangeEnt::point(op.key);
        if(query.empty()) continue;
        if(!index.hierarchy_cover_into(query,scratch)) return false;
        for(const auto& node:scratch) {
            // The root intention is the phase-0 ancestor lock. A non-leaf
            // root is not repeated as a separator resource; a leaf root is
            // retained because it is also the only range-cover object.
            if(node.depth==0 && !node.leaf) continue;
            const unsigned node_phase=static_cast<unsigned>(node.depth+1);
            if(scan) {
                // A fully covered interior span is the separator-level lock.
                // Boundary leaves remain explicit so gaps are covered too.
                if(!node.leaf && separator_range_contains(query,node.range))
                    add(LockMode::S,node.range,LockKind::Node,node_phase);
                else if(node.leaf)
                    add(LockMode::S,node.range,LockKind::Node,node_phase);
            } else {
                // Point access keeps separator ancestors intention-only and
                // protects the canonical row separately.
                add(write?LockMode::IX:LockMode::IS,node.range,LockKind::Node,node_phase);
            }
        }
        if(!scan) add(write?LockMode::X:LockMode::S,KeyRangeEnt::point(op.key),LockKind::Record,
                       std::numeric_limits<unsigned>::max());
    }
    std::sort(plan.begin(),plan.end(),[](const HierarchyRequest& lhs,const HierarchyRequest& rhs) {
        return lhs.phase!=rhs.phase ? lhs.phase<rhs.phase :
               lhs.range.StartKey!=rhs.range.StartKey ? lhs.range.StartKey<rhs.range.StartKey :
               lhs.range.EndKey<rhs.range.EndKey;
    });
    return true;
}

inline bool covers_separator_plan(const std::vector<HierarchyRequest>& held,
                                  const std::vector<HierarchyRequest>& current) {
    for(const auto& r:current) {
        auto found=std::find_if(held.begin(),held.end(),[&](const HierarchyRequest& h) {
            return h.kind==r.kind && h.phase==r.phase && same_separator_range(h.range,r.range);
        });
        if(found==held.end() || lock_mode_join(found->mode,r.mode)!=found->mode) return false;
    }
    return true;
}
} // namespace rrl::detail
