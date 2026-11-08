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
// Gray et al. (1976): ancestor intentions before descendant S/X.
// Physical node naming + strict split protection are project adaptations,
// not Graefe's logical separator-key protocol (see the reproduction audit).
// Every newly published child remains behind a pre-existing X ancestor.
// This is Deneva's leaf/parent CAS idea with recursive split protection and
// strict lifetime. No global topology resource and no index/lock callbacks.
inline bool leaf_plan(BTreeIndex& index,const TransactionTrace& txn,std::vector<IndexNodeSpan>& scratch,
                       std::vector<HierarchyRequest>& plan,bool deneva=false) {
    plan.clear();
    auto add=[&](LockMode mode,std::uint64_t id,LockKind kind,unsigned phase) {
        auto found=std::find_if(plan.begin(),plan.end(),[&](const HierarchyRequest& r){return r.kind==kind && r.range.StartKey==id;});
        if(found!=plan.end()){found->mode=lock_mode_join(found->mode,mode);return;}
        if(plan.size()==plan.capacity())throw std::runtime_error("hierarchy plan workspace capacity");
        plan.push_back({mode,KeyRangeEnt::point(id),kind,phase});
    };
    std::size_t fresh_budget=0,delete_budget=0;
    for(const auto& op:txn.operations) {
        if(op.kind==OperationKind::Insert || op.kind==OperationKind::Update)++fresh_budget;
        if(op.kind==OperationKind::Delete)++delete_budget;
    }
    const bool writing=fresh_budget || delete_budget;
    add(writing?LockMode::IX:LockMode::IS,0,LockKind::Node,0);
    for(const auto& op:txn.operations) {
        const bool scan=op.kind==OperationKind::RangeRead,write=op.kind!=OperationKind::Read && !scan;
        const auto range=scan?KeyRangeEnt::half_open(op.key,op.end_key):KeyRangeEnt::point(op.key);
        if(!index.hierarchy_cover_into(range,scratch))return false;
        for(const auto& node:scratch)
            add(node.leaf && scan?LockMode::S:write?LockMode::IX:LockMode::IS,node.id,LockKind::Node,node.depth+1);
        if(!scan && !(deneva && op.kind==OperationKind::Insert))add(write?LockMode::X:LockMode::S,op.key,LockKind::Record,std::numeric_limits<unsigned>::max());
        if(scan || !write)continue;
        const bool removing=op.kind==OperationKind::Delete;
        const bool inserting=op.kind==OperationKind::Insert ||
            (op.kind==OperationKind::Update && (scratch.empty() || !scratch.back().start_key_present));
        if(!removing && !inserting)continue;
        if(deneva && op.kind==OperationKind::Insert) {
            if(scratch.empty())add(LockMode::X,0,LockKind::Node,0);
            else add(LockMode::X,scratch.back().id,LockKind::Node,scratch.back().depth+1);
            continue;
        }
        const IndexNodeSpan* safe=nullptr;
        for(auto it=scratch.rbegin();it!=scratch.rend();++it) {
            // Point snapshots are exactly one root-to-leaf path.
            const bool spare=it->keys<=it->max_keys && fresh_budget<=it->max_keys-it->keys;
            if(removing) {
                // Survive all possible child removals. Keep the lower fence
                // key outside the delete set so rerouting cannot escape X.
                const bool stable_lower=!it->range.has_start || it->range.StartKey==0 ||
                    std::none_of(txn.operations.begin(),txn.operations.end(),[&](const Operation& other){
                        return other.kind==OperationKind::Delete && other.key==it->range.StartKey;
                    });
                const bool root_survives=it->depth!=0 ||
                    (it->children>1 && delete_budget<it->children-1);
                if(!it->leaf && it->children>delete_budget && root_survives && spare && stable_lower){safe=&*it;break;}
            }else if(spare){safe=&*it;break;}
        }
        if(safe)add(LockMode::X,safe->id,LockKind::Node,safe->depth+1);
        else add(LockMode::X,0,LockKind::Node,0); // Only a possible root replacement.
        if(!scratch.empty())add(LockMode::X,scratch.back().id,LockKind::Node,scratch.back().depth+1);
    }
    std::sort(plan.begin(),plan.end(),[](const HierarchyRequest& a,const HierarchyRequest& b){
        return a.phase!=b.phase?a.phase<b.phase:a.range.StartKey<b.range.StartKey;
    });
    return true;
}
inline bool covers_hierarchy_plan(const std::vector<HierarchyRequest>& held,
                                 const std::vector<HierarchyRequest>& current) {
    for(const auto& r:current) {
        auto h=std::find_if(held.begin(),held.end(),[&](const HierarchyRequest& value){return r.kind==value.kind && r.range.StartKey==value.range.StartKey;});
        if(h==held.end() || lock_mode_join(h->mode,r.mode)!=h->mode)return false;
    }
    return true;
}
}
