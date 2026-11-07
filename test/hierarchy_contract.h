#pragma once
#include "../src/txn/hierarchical_plan.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace hierarchy_test {
using namespace rrl;
inline void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; std::exit(1); }
}
inline std::vector<detail::HierarchyRequest> point_plan(idx_key_t k, bool write) {
    TransactionTrace t{1, {{write ? OperationKind::Update : OperationKind::Read, k, k, 1}}};
    return detail::hierarchy_plan(t, 256);
}
inline RC acquire(IRangeLockManager& m, txnid_t txn, const std::vector<detail::HierarchyRequest>& p) {
    for (const auto& r : p) {
        auto rc = m.lock(txn, r.mode, r.range, r.kind);
        if (rc != RCOK) { check(m.unlock_all(txn) == RCOK, "partial plan cleanup"); return rc; }
    }
    return RCOK;
}
inline void contract(IRangeLockManager& m) {
    const auto b = KeyRangeEnt::closed(0,255);
    auto release = [&](txnid_t t) { check(m.unlock_all(t) == RCOK, "release"); };
    auto plan1 = point_plan(10, true), plan2 = point_plan(20, true);
    check(plan1.size() == 3 && plan1[1].mode == LockMode::IX && plan1[2].kind == LockKind::Record,
          "point write retains record granularity");
    check(acquire(m, 1, plan1) == RCOK && acquire(m, 2, plan2) == RCOK,
          "different records in one bucket can both hold X");
    check(acquire(m, 3, point_plan(10, false)) == AbortConflict, "same record read/write conflict");
    release(1); release(2);

    // Even an empty result range must exclude a new key in its bucket.
    TransactionTrace scan{1, {{OperationKind::RangeRead, 12, 18, 0}}};
    const auto scan_plan = detail::hierarchy_plan(scan, 256);
    check(acquire(m, 4, scan_plan) == RCOK, "range plan");
    check(acquire(m, 5, point_plan(15, true)) == AbortConflict, "empty-result phantom insert prevented");
    check(acquire(m, 6, point_plan(270, true)) == RCOK, "different bucket remains writable");
    release(6); release(4);
    check(acquire(m, 5, point_plan(15, true)) == RCOK, "writer proceeds after scan ends"); release(5);

    // Reverse arrival: a held record X excludes the scan at bucket S.
    check(acquire(m, 7, plan1) == RCOK, "writer first");
    check(acquire(m, 8, scan_plan) == AbortConflict, "scan sees existing bucket IX"); release(7);
    // Bucket IS must not conflict with an unrelated record X, in either direction.
    check(acquire(m, 9, point_plan(10, false)) == RCOK && acquire(m, 10, plan2) == RCOK,
          "point read and distinct point write coexist"); release(9); release(10);

    TransactionTrace boundary{1, {{OperationKind::RangeRead, 255, 256, 0}}};
    auto boundary_plan = detail::hierarchy_plan(boundary, 256);
    check(boundary_plan.size() == 2, "half-open endpoint does not cover next bucket");
    check(acquire(m, 11, boundary_plan) == RCOK && acquire(m, 12, point_plan(256,true)) == RCOK,
          "exclusive upper endpoint remains writable"); release(11); release(12);
    // S+IX is SIX, which is compatible with a peer's IS but not S/IX.
    check(m.lock(20,LockMode::S,b,LockKind::Node)==RCOK,"upgrade S setup");
    check(m.lock(21,LockMode::IS,b,LockKind::Node)==RCOK,"upgrade IS peer");
    check(m.lock(20,LockMode::IX,b,LockKind::Node)==RCOK,"S to SIX preserves IS peer concurrency");
    check(m.lock(22,LockMode::S,b,LockKind::Node)==AbortConflict,"SIX excludes peer covering S");
    check(m.lock(22,LockMode::IX,b,LockKind::Node)==AbortConflict,"SIX excludes peer IX");
    check(m.lock(21,LockMode::S,KeyRangeEnt::point(11),LockKind::Record)==RCOK,"SIX subtree S allows child S");
    release(20);release(21);release(22);
    check(m.get_lock_count() == 0, "no leaked hierarchy locks");
    // Width-one bucket identity and record identity have equal ranges but
    // different kinds. IX must still allow a distinct record's S/X.
    TransactionTrace singleton{1, {{OperationKind::Update,1000,1000,0}}};
    auto one = detail::hierarchy_plan(singleton,1);
    check(acquire(m,30,one)==RCOK,"width-one writer");
    singleton.operations[0].key=1001;
    check(acquire(m,31,detail::hierarchy_plan(singleton,1))==RCOK,"width-one neighbor");
    release(30); release(31);
    check(m.get_lock_count()==0,"width-one cleanup");
}
inline void plan_contract() {
    using K=OperationKind;
    TransactionTrace txn{1, {{K::Read,30,30,0},{K::Update,30,30,1},
                           {K::Update,10,10,2},{K::Read,300,300,0}}};
    const auto p = detail::hierarchy_plan(txn, 256);
    check(p.size()==6 && p[0].mode==LockMode::IX && p[1].range.StartKey==0 &&
          p[2].range.StartKey==256 && p[3].range.StartKey==10 &&
          p[4].range.StartKey==30 && p[4].mode==LockMode::X && p[5].mode==LockMode::S,
          "strongest modes, deduplication, root/bucket/record order");
    txn.operations={{K::RangeRead,10,20,0},{K::Update,15,15,1}};
    const auto mixed=detail::hierarchy_plan(txn,256);
    check(mixed.size()==3 && mixed[1].mode==LockMode::SIX && mixed[2].mode==LockMode::X && mixed[2].kind==LockKind::Record,
          "mixed scan/write requests SIX plus actual record X");
    txn.operations={{K::RangeRead,20,20,0}};
    check(detail::hierarchy_plan(txn,256).size()==1, "empty interval takes no data bucket");
    const auto max=std::numeric_limits<idx_key_t>::max();
    txn.operations={{K::Update,max,max,0}};
    auto end=detail::hierarchy_plan(txn,256);
    check(end[1].range.EndKey==max && end[2].range.StartKey==max,"maximum key does not wrap");
    end=detail::hierarchy_plan(txn,1);
    check(end[1].range.StartKey==max && end[1].range.EndKey==max,"width one has valid bucket identity");
}
} // namespace hierarchy_test
