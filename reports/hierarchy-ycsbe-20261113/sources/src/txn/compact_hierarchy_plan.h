#pragma once
#include "hierarchical_plan.h"
namespace rrl::detail {
// Compile the existing logical plan to stable array coordinates once. Bucket
// and record identities remain distinct even for width-one partitions.
struct CompactHierarchyPlans {
    struct Request {std::uint32_t node;LockMode mode;};
    std::vector<std::vector<Request>> plans;
    std::size_t nodes{1},max_locks{1},directory_bytes{0},plan_bytes{0};
    CompactHierarchyPlans(const WorkloadTrace& trace,bool implicit_root) {
        std::vector<idx_key_t> buckets,records;
        std::vector<std::vector<HierarchyRequest>> logical;
        for(const auto& txn:trace.transactions()) {
            logical.push_back(hierarchy_plan(txn,trace.config().hierarchy_bucket_width));
            for(const auto& r:logical.back()) {
                if(r.phase==1)buckets.push_back(r.range.StartKey);
                else if(r.phase==2)records.push_back(r.range.StartKey);
            }
        }
        auto normalize=[](auto& v){std::sort(v.begin(),v.end());v.erase(std::unique(v.begin(),v.end()),v.end());};
        normalize(buckets);normalize(records);
        nodes=1+buckets.size()+records.size();
        if(nodes>UINT32_MAX)throw std::invalid_argument("compact hierarchy directory too large");
        plans.reserve(logical.size());
        for(const auto& source:logical) {
            plans.emplace_back();auto& plan=plans.back();plan.reserve(source.size());
            for(const auto& r:source) {
                if(r.phase==0) {
                    if(r.mode!=LockMode::IS && r.mode!=LockMode::IX)throw std::invalid_argument("invalid implicit root plan");
                    if(!implicit_root)plan.push_back({0,r.mode});
                } else {
                    const auto& directory=r.phase==1?buckets:records;
                    const auto rank=std::lower_bound(directory.begin(),directory.end(),r.range.StartKey)-directory.begin();
                    const auto node=1+(r.phase==1?0:buckets.size())+rank;
                    plan.push_back({static_cast<std::uint32_t>(node),r.mode});
                }
            }
            max_locks=std::max(max_locks,plan.size());plan_bytes+=plan.capacity()*sizeof(Request);
        }
        plan_bytes+=plans.capacity()*sizeof(std::vector<Request>);
        // Coordinate directories are discarded: all executable requests now
        // contain resource IDs. No runtime endpoint search remains.
    }
};
} // namespace rrl::detail
