#pragma once
#include "hierarchical_plan.h"
#include <limits>
namespace rrl::detail {
inline std::vector<idx_key_t> object_record_keys(const WorkloadTrace& trace) {
    std::vector<idx_key_t> keys;keys.reserve(trace.config().key_count);
    for(std::size_t key=0;key<trace.config().key_count;++key)keys.push_back(key);
    for(const auto& txn:trace.transactions())for(const auto& op:txn.operations)
        if(op.key>=trace.config().key_count)keys.push_back(op.key);
    std::sort(keys.begin(),keys.end());keys.erase(std::unique(keys.begin(),keys.end()),keys.end());return keys;
}
inline bool topology_write(const TransactionTrace& txn) {
    return std::any_of(txn.operations.begin(),txn.operations.end(),[](const Operation& op){return op.kind==OperationKind::Insert || op.kind==OperationKind::Delete;});
}
inline bool leaf_plan(BTreeIndex& index,const TransactionTrace& txn,std::vector<LeafSpan>& scratch,
                       std::vector<HierarchyRequest>& plan) {
    plan.clear();
    auto add=[&](LockMode mode,std::uint64_t id,LockKind kind,unsigned phase) {
        auto found=std::find_if(plan.begin(),plan.end(),[&](const HierarchyRequest& r){return r.kind==kind && r.range.StartKey==id;});
        if(found!=plan.end()){found->mode=lock_mode_join(found->mode,mode);return;}
        if(plan.size()==plan.capacity())throw std::runtime_error("leaf plan workspace capacity");
        plan.push_back({mode,KeyRangeEnt::point(id),kind,phase});
    };
    for(const auto& op:txn.operations) {
        const bool scan=op.kind==OperationKind::RangeRead,write=op.kind!=OperationKind::Read && !scan;
        const auto range=scan?KeyRangeEnt::half_open(op.key,op.end_key):KeyRangeEnt::point(op.key);
        if(!index.leaf_cover_into(range,scratch))return false;
        for(const auto& leaf:scratch)add(scan?LockMode::S:write?LockMode::IX:LockMode::IS,leaf.id,LockKind::Node,1);
        if(!scan)add(write?LockMode::X:LockMode::S,op.key,LockKind::Record,2);
    }
    // No upgrades: scan S + write IX becomes X before any leaf is acquired.
    std::sort(plan.begin(),plan.end(),[](const HierarchyRequest& a,const HierarchyRequest& b){
        return a.phase!=b.phase?a.phase<b.phase:a.range.StartKey<b.range.StartKey;
    });
    return true;
}
}
