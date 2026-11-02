#include "txn/workload.h"
#include <cstdlib>
#include <iostream>
#include <set>
using namespace rrl;

namespace {
struct FaultIndex : BTreeIndex {
    BTreeLatch tree;
    std::size_t calls{0}, fail_at{0};
    FaultIndex() { tree.init(8); }
    RC init(std::size_t order) override { return tree.init(order); }
    RC insert(idx_key_t key, void* value) override {
        if (++calls == fail_at) return ERROR;
        return tree.insert(key, value);
    }
    RC erase(idx_key_t key) override { return tree.erase(key); }
    bool lookup(idx_key_t key, void*& value) const override { return tree.lookup(key, value); }
    std::vector<KeyValue> scan_range(const KeyRangeEnt& range) const override { return tree.scan_range(range); }
    std::size_t size() const override { return tree.size(); }
};
struct FaultLocks : IRangeLockManager {
    int calls{0}, releases{0}, fail_at{0};
    RC lock_rc{ERROR}, release_rc{RCOK};
    RC lock(txnid_t, LockMode, const KeyRangeEnt&, LockKind,
            std::vector<txnid_t>*) override { return ++calls == fail_at ? lock_rc : RCOK; }
    RC unlock(txnid_t, const KeyRangeEnt&, LockKind) override { return release_rc; }
    RC unlock_all(txnid_t) override { ++releases; return release_rc; }
    // Deliberately uninformative: cleanup must track acquisitions, not count().
    std::size_t get_lock_count(txnid_t) const override { return 0; }
};
void check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; std::exit(1); }
}
}
int main() {
    WorkloadConfig config;
    config.kind = WorkloadKind::TpccPayment;
    config.key_count = 1;
    config.transaction_count = 10;
    const auto trace = WorkloadTrace::generate(config);
    using Driver = RunStats (*)(BTreeIndex&, IRangeLockManager&, const WorkloadTrace&, size_t);
    for (Driver run : {run_next_key_lock, run_hierarchical_lock}) {
        for (int fail_at : {1, 2}) {
            FaultIndex index;
            FaultLocks locks;
            locks.fail_at = fail_at;
            const auto stats = run(index, locks, trace, 1);
            check(stats.failed() && stats.lock_errors == 1 && stats.lock_conflicts == 0,
                  "hard lock error must not become conflict");
            check(stats.transactions == 1 && stats.failed_transactions == 1 && stats.committed == 0,
                  "hard error stops new transactions");
            check(locks.releases == 1, "cleanup after partial/uncertain acquisition");
        }
        for (RC release_rc : {ERROR, AbortGate, Abort}) {
            FaultIndex index;
            FaultLocks locks;
            locks.release_rc = release_rc;
            const auto stats = run(index, locks, trace, 1);
            check(stats.failed() && stats.lock_release_failures == 1 && stats.transactions == 1 &&
                  stats.failed_transactions == 1, "release failure invalidates run");
        }
        for (RC conflict : {AbortConflict, AbortCapacity, AbortGate}) {
            FaultIndex index;
            FaultLocks locks;
            locks.fail_at = 1;
            locks.lock_rc = conflict;
            const auto stats = run(index, locks, trace, 1);
            check(!stats.failed() && stats.transactions == 10 && stats.aborted == 1,
                  "ordinary abort does not stop run");
        }
    }
    std::set<idx_key_t> seed_keys;
    for (size_t k = 0; k < config.key_count; ++k) seed_keys.insert(k);
    for (const auto& txn : trace.transactions())
        for (const auto& op : txn.operations) seed_keys.insert(op.key);
    for (int failed_write : {1, 2}) for (int driver = 0; driver < 5; ++driver) {
        FaultIndex index;
        // Cover errors both before and after a prefix of writes was applied.
        index.fail_at = seed_keys.size() + failed_write;
        LockManager locks;
        RunStats stats;
        if (driver == 0) stats = run_workload(index, trace, 1);
        if (driver == 1) stats = run_record_2pl(index, locks, trace, 1);
        if (driver == 2) stats = run_gap_lock(index, locks, trace, 1);
        if (driver == 3) stats = run_next_key_lock(index, locks, trace, 1);
        if (driver == 4) stats = run_hierarchical_lock(index, locks, trace, 1);
        check(stats.failed() && stats.index_errors == 1 && stats.committed == 0 &&
              stats.transactions == 1 && stats.failed_transactions == 1, "all drivers check index writes");
        check(locks.get_lock_count() == 0, "index failure releases acquired locks");
    }
    FaultIndex seed_failure;
    seed_failure.fail_at = 1;
    const auto stats = run_workload(seed_failure, trace, 1);
    check(stats.failed() && stats.index_errors == 1 && stats.transactions == 0, "seed failure is surfaced");
    std::cout << "workload errors: OK\n";
}
