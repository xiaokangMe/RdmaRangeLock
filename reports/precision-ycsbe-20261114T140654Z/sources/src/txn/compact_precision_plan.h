#pragma once
#include "txn/workload.h"
#include <algorithm>
#include <limits>

namespace rrl::detail {
// Fixed-admission single-key-interval Precision Locking, not a general online
// predicate service. Only admitted update keys can ever enter Lu. Register Lp
// on exactly those update keys satisfying the predicate; read-only keys have
// no possible Lu counterpart. This changes representation, not conflict rules.
// No schedule/conflict outcome is consulted, and operations are NOT prelocked.
struct CompactPrecisionPlans {
    struct OperationPlan {
        std::uint32_t first{0},end{0};
        // X encodes the inseparable point Lp + point Lu of a write. Two Lu
        // entries alone do not conflict; each writer's implicit Lp causes it.
        LockMode mode{LockMode::S};
    };
    std::vector<std::vector<OperationPlan>> operations;
    std::size_t update_keys{0},max_locks{1},plan_bytes{0};

    explicit CompactPrecisionPlans(const WorkloadTrace& trace) {
        std::vector<idx_key_t> keys;
        for(const auto& txn:trace.transactions())for(const auto& op:txn.operations)
            if(op.kind!=OperationKind::Read && op.kind!=OperationKind::RangeRead)keys.push_back(op.key);
        std::sort(keys.begin(),keys.end());
        keys.erase(std::unique(keys.begin(),keys.end()),keys.end());
        update_keys=keys.size();
        if(update_keys>=std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument("compact precision update directory too large");
        const auto lower=[&](idx_key_t key){return static_cast<std::uint32_t>(std::lower_bound(keys.begin(),keys.end(),key)-keys.begin());};
        operations.reserve(trace.transactions().size());
        for(const auto& txn:trace.transactions()) {
            std::vector<OperationPlan> plan;plan.reserve(txn.operations.size());
            std::size_t upper_bound=0;
            for(const auto& op:txn.operations) {
                OperationPlan request;
                request.first=lower(op.key);request.end=request.first;
                if(op.kind==OperationKind::RangeRead) {
                    if(op.key<op.end_key)request.end=lower(op.end_key);
                } else {
                    if(request.first<keys.size() && keys[request.first]==op.key)++request.end;
                    if(op.kind!=OperationKind::Read)request.mode=LockMode::X;
                }
                upper_bound+=request.end-request.first;
                plan.push_back(request);
            }
            max_locks=std::max(max_locks,std::min(upper_bound,update_keys));
            plan_bytes+=plan.capacity()*sizeof(OperationPlan);
            operations.push_back(std::move(plan));
        }
        plan_bytes+=operations.capacity()*sizeof(std::vector<OperationPlan>);
        // The immutable plans contain compact IDs, so keys can now be freed.
    }
};
} // namespace rrl::detail
