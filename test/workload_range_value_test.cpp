#include "txn/workload.h"
#include <cstdlib>
#include <iostream>
#include <functional>
using namespace rrl;
namespace {
void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; std::exit(1); }
}
struct ScanWindowIndex : BTreeIndex {
    mutable BTreeLatch tree;
    mutable bool fired{false}, prepared{false};
    std::function<void()> window;
    idx_key_t target{0};
    bool key_neighbors(idx_key_t k,IndexKeyNeighbors& out) const override {
        // The hole must predate the neighbor snapshot, otherwise deleting
        // and restoring the same key is a valid ABA with locked value reread.
        if(!prepared){prepared=true;check(tree.erase(target)==RCOK,"prepare missing boundary before observation");}
        return tree.key_neighbors(k,out);
    }
    bool key_range_into(const KeyRangeEnt& range,std::vector<idx_key_t>& out) const override{return tree.key_range_into(range,out);}
    RC init(std::size_t order) override { return tree.init(order); }
    RC insert(idx_key_t key, void* value) override { return tree.insert(key, value); }
    RC erase(idx_key_t key) override { return tree.erase(key); }
    bool lookup(idx_key_t key, void*& value) const override { return tree.lookup(key, value); }
    std::size_t size() const override { return tree.size(); }
    std::vector<KeyValue> scan_range(const KeyRangeEnt& range) const override {
        if (!fired) {
            fired = true;
            if(!prepared){prepared=true;check(tree.erase(target)==RCOK,"prepare insertion gap before first scan");}
            auto candidates = tree.scan_range(range);
            window(); // Writer attempts insertion after enumeration.
            return candidates;
        }
        return tree.scan_range(range);
    }
};
struct WindowLocks : LockManager {
    BTreeIndex& tree;
    idx_key_t target;
    std::uint64_t replacement{0x12345};
    bool fired{false};
    WindowLocks(BTreeIndex& index, idx_key_t key) : tree(index), target(key) {}
    RC lock(txnid_t tid, LockMode mode, const KeyRangeEnt& range, LockKind kind,
            std::vector<txnid_t>* conflicts) override {
        if (!fired) {
            fired = true;
            // Commit a writer after candidate enumeration but before the
            // reader's first lock. No scheduler timing or sleeps are involved.
            check(LockManager::lock(999, LockMode::X, KeyRangeEnt::point(target), LockKind::Record) == RCOK,
                  "writer can enter the scan-to-lock window");
            check(tree.insert(target, &replacement) == RCOK, "writer updates value");
            check(LockManager::unlock_all(999) == RCOK, "writer commits and releases");
        }
        return LockManager::lock(tid, mode, range, kind, conflicts);
    }
};
}
int main() {
    WorkloadConfig config;
    config.kind = WorkloadKind::YcsbE;
    config.key_count = 8; config.transaction_count = 1;
    config.operations_per_transaction = 1; config.range_length = 4;
    auto trace = WorkloadTrace::generate(config);
    while (trace.transactions()[0].operations[0].kind != OperationKind::RangeRead) {
        ++config.seed; trace = WorkloadTrace::generate(config);
    }
    const auto target = trace.transactions()[0].operations[0].key;
    for (int driver = 0; driver < 4; ++driver) {
        BTreeLatch tree;
        tree.init(8);
        WindowLocks locks(tree, target);
        RunStats stats;
        if (driver == 0) stats = run_record_2pl(tree, locks, trace, 1);
        if (driver == 1) stats = run_gap_lock(tree, locks, trace, 1);
        if (driver == 2) stats = run_next_key_lock(tree, locks, trace, 1);
        if (driver == 3) stats = run_hierarchical_lock(tree, locks, trace, 1);
        check(locks.fired && stats.committed == 1 && !stats.failed(), "reader commits after writer");
        if (stats.checksum != (target ^ locks.replacement)) {
            std::cerr << "driver=" << driver << " checksum=" << stats.checksum << "\n";
            check(false, "committed range consumes post-lock value");
        }
        check(locks.get_lock_count() == 0, "strict-2PL release after completion");
    }
    for (int driver = 0; driver < 3; ++driver) {
        ScanWindowIndex index;
        index.init(8); index.target = target;
        LockManager locks;
        RC writer_rc = ERROR;
        std::uint64_t inserted = 77;
        index.window = [&] {
            writer_rc = locks.lock(999, LockMode::X, KeyRangeEnt::point(target), LockKind::InsertIntent);
            if (writer_rc == RCOK) {
                check(index.insert(target, &inserted) == RCOK, "insert in scan window");
                check(locks.unlock_all(999) == RCOK, "inserting writer commits");
            }
        };
        RunStats stats;
        if (driver == 0) stats = run_record_2pl(index, locks, trace, 1);
        if (driver == 1) stats = run_gap_lock(index, locks, trace, 1);
        if (driver == 2) stats = run_next_key_lock(index, locks, trace, 1);
        if (driver == 0) check(writer_rc == RCOK && stats.committed == 1,
                               "record-only baseline retains phantom window");
        if (driver == 1) check(writer_rc == RCOK && stats.aborted == 1 && stats.committed == 0,
                               "gap rejects changed physical boundary snapshot");
        if (driver == 2) check(writer_rc == RCOK && stats.aborted == 1 && stats.committed == 0,
                               "next-key rejects changed candidate keys");
        check(!stats.failed() && locks.get_lock_count() == 0, "window test completes and releases");
    }
    std::cout << "range value window: OK\n";
}
