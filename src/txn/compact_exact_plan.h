#pragma once
#include "txn/workload.h"
#include <algorithm>
#include <limits>

namespace rrl::detail {
// Partition the integer key domain at every admitted request boundary. Presence
// in the B+ tree is irrelevant: empty intervals have the same lock identity.
// Plans depend only on request geometry, never on conflicts or execution order.
struct CompactExactPlans {
    struct Request {std::uint32_t node;LockMode mode;};
    std::vector<std::vector<Request>> plans;
    std::size_t nodes{1},max_locks{1},plan_bytes{0};

    explicit CompactExactPlans(const WorkloadTrace& trace) {
        std::vector<idx_key_t> cuts{0};
        for(const auto& txn:trace.transactions())for(const auto& op:txn.operations) {
            if(op.kind==OperationKind::RangeRead) {
                if(op.key>=op.end_key)continue;
                cuts.push_back(op.key);cuts.push_back(op.end_key);
            } else {
                cuts.push_back(op.key);
                if(op.key!=std::numeric_limits<idx_key_t>::max())cuts.push_back(op.key+1);
            }
        }
        std::sort(cuts.begin(),cuts.end());cuts.erase(std::unique(cuts.begin(),cuts.end()),cuts.end());
        nodes=cuts.size();
        if(nodes>UINT32_MAX)throw std::invalid_argument("compact exact directory too large");
        auto rank=[&](idx_key_t key){return static_cast<std::size_t>(std::lower_bound(cuts.begin(),cuts.end(),key)-cuts.begin());};
        plans.reserve(trace.transactions().size());
        for(const auto& txn:trace.transactions()) {
            std::vector<Request> requests;
            for(const auto& op:txn.operations) {
                if(op.kind==OperationKind::RangeRead) {
                    if(op.key>=op.end_key)continue;
                    const auto end=rank(op.end_key);
                    for(auto n=rank(op.key);n<end;++n)requests.push_back({static_cast<std::uint32_t>(n),LockMode::S});
                } else requests.push_back({static_cast<std::uint32_t>(rank(op.key)),
                    op.kind==OperationKind::Read?LockMode::S:LockMode::X});
            }
            std::sort(requests.begin(),requests.end(),[](const Request& a,const Request& b){return a.node<b.node;});
            std::size_t unique=0;
            for(const auto& request:requests) {
                if(unique && requests[unique-1].node==request.node) {
                    if(request.mode==LockMode::X)requests[unique-1].mode=LockMode::X;
                } else requests[unique++]=request;
            }
            requests.resize(unique);
            // Drop duplicate scratch capacity before entering the timed run.
            plans.emplace_back(requests.begin(),requests.end());
            max_locks=std::max(max_locks,unique);
            plan_bytes+=plans.back().capacity()*sizeof(Request);
        }
        plan_bytes+=plans.capacity()*sizeof(std::vector<Request>);
        // All executable requests now use integer IDs; discard cuts entirely.
    }
};
} // namespace rrl::detail
