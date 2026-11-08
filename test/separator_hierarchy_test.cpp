#include "index/btree.h"
#include "lock/lock_manager.h"
#include "../src/txn/hierarchical_plan.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <vector>

using namespace rrl;

static bool has_phase(const std::vector<detail::HierarchyRequest>& plan,unsigned phase) {
    return std::any_of(plan.begin(),plan.end(),[&](const auto& r){return r.phase==phase;});
}

static bool phase_ordered(const std::vector<detail::HierarchyRequest>& plan) {
    return std::is_sorted(plan.begin(),plan.end(),[](const auto& lhs,const auto& rhs) {
        return lhs.phase < rhs.phase;
    });
}

int main() {
    // Endpoint containment is asymmetric at an open/closed left fence.
    if (!detail::separator_range_contains(KeyRangeEnt::from_left(10,Bound::Closed),
                                          KeyRangeEnt::from_left(10,Bound::Open))) return 1;
    if (detail::separator_range_contains(KeyRangeEnt::from_left(10,Bound::Open),
                                         KeyRangeEnt::from_left(10,Bound::Closed))) return 2;
    if (!detail::separator_range_contains(KeyRangeEnt::to_right(20,Bound::Closed),
                                          KeyRangeEnt::to_right(20,Bound::Open))) return 3;
    if (detail::separator_range_contains(KeyRangeEnt::to_right(20,Bound::Open),
                                         KeyRangeEnt::to_right(20,Bound::Closed))) return 4;

    BTreeLatch tree;
    if (tree.init(4) != RCOK) return 5;
    std::vector<std::uint64_t> values(128);
    for (idx_key_t key : {10,20,30,40,50,60,70,80,90}) {
        values[key]=key;
        if (tree.insert(key,&values[key]) != RCOK) return 6;
    }

    TransactionTrace scan;
    scan.operations.push_back({OperationKind::RangeRead,15,75,0});
    std::vector<IndexNodeSpan> scratch;
    scratch.reserve(256);
    std::vector<detail::HierarchyRequest> plan;
    if (!detail::separator_plan(tree,scan,scratch,plan)) return 7;
    if (plan.empty() || !has_phase(plan,0) ||
        !std::any_of(plan.begin(),plan.end(),[](const auto& r){return r.phase>0;})) return 8;
    if (has_phase(plan,std::numeric_limits<unsigned>::max())) return 9; // no record lock for a scan
    if (!phase_ordered(plan)) return 10;

    std::vector<detail::HierarchyRequest> check;
    if (!detail::separator_plan(tree,scan,scratch,check) ||
        !detail::covers_separator_plan(plan,check)) return 11;

    TransactionTrace update;
    update.operations.push_back({OperationKind::Update,40,0,9});
    std::vector<detail::HierarchyRequest> write_plan;
    if (!detail::separator_plan(tree,update,scratch,write_plan)) return 12;
    if (!has_phase(write_plan,0) || !has_phase(write_plan,std::numeric_limits<unsigned>::max())) return 13;
    if (!phase_ordered(write_plan)) return 14;

    TransactionTrace mixed;
    mixed.operations.push_back({OperationKind::RangeRead,15,75,0});
    mixed.operations.push_back({OperationKind::Update,40,0,9});
    std::vector<detail::HierarchyRequest> mixed_plan;
    if (!detail::separator_plan(tree,mixed,scratch,mixed_plan)) return 15;
    if (!std::any_of(mixed_plan.begin(),mixed_plan.end(),[](const auto& r){return r.mode==LockMode::SIX;})) return 16;

    LockManager locks;
    for (const auto& request : plan)
        if (locks.lock(1,request.mode,request.range,request.kind) != RCOK) return 17;
    bool saw_conflict=false;
    for (const auto& request : write_plan) {
        const auto rc=locks.lock(2,request.mode,request.range,request.kind);
        if (rc==Abort) saw_conflict=true;
        else if (rc!=RCOK) return 18;
    }
    if (!saw_conflict) return 19;
    locks.unlock_all(1); locks.unlock_all(2);

    // Empty-result and UINT64_MAX queries still receive a structural cover.
    TransactionTrace empty;
    empty.operations.push_back({OperationKind::RangeRead,95,100,0});
    std::vector<detail::HierarchyRequest> empty_plan;
    if (!detail::separator_plan(tree,empty,scratch,empty_plan) || empty_plan.empty()) return 20;
    TransactionTrace high;
    high.operations.push_back({OperationKind::RangeRead,
                               std::numeric_limits<idx_key_t>::max()-1,
                               std::numeric_limits<idx_key_t>::max(),0});
    std::vector<detail::HierarchyRequest> high_plan;
    if (!detail::separator_plan(tree,high,scratch,high_plan) || high_plan.empty()) return 21;

    // A split publishes new separator spans; the stale plan must be rejected.
    BTreeLatch split_tree;
    if (split_tree.init(3) != RCOK) return 22;
    for (idx_key_t key : {10,20,30}) {
        values[key]=key;
        if (split_tree.insert(key,&values[key]) != RCOK) return 23;
    }
    TransactionTrace whole;
    whole.operations.push_back({OperationKind::RangeRead,0,100,0});
    std::vector<IndexNodeSpan> split_scratch;
    split_scratch.reserve(256);
    std::vector<detail::HierarchyRequest> stale;
    if (!detail::separator_plan(split_tree,whole,split_scratch,stale)) return 24;
    values[40]=40;
    if (split_tree.insert(40,&values[40]) != RCOK) return 25;
    std::vector<detail::HierarchyRequest> after_split;
    if (!detail::separator_plan(split_tree,whole,split_scratch,after_split)) return 26;
    if (detail::covers_separator_plan(stale,after_split)) return 27;

    // Removing an entire child can collapse the root; revalidation must fail.
    if (split_tree.erase(10) != RCOK || split_tree.erase(20) != RCOK) return 28;
    std::vector<detail::HierarchyRequest> after_erase;
    if (!detail::separator_plan(split_tree,whole,split_scratch,after_erase)) return 29;
    if (detail::covers_separator_plan(after_split,after_erase)) return 30;

    std::cout << "separator hierarchy plan, boundaries, ordering and topology validation: OK\n";
    return 0;
}
