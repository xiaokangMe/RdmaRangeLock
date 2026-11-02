// Integration test for the remote lock manager against a live lock server.
//
// It is skipped unless RRL_TEST_RDMA_HOST and RRL_TEST_RDMA_PORT are set, so a
// clean clone without RDMA hardware still builds and passes the suite. The
// point is to cover the paths that only exist at runtime: gate acquisition and
// retry, cache bookkeeping across lock/unlock, and the failure snapshot.

#include "lock/remote_lock_manager.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace rrl;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

}  // namespace

int main() {
    const char* host = std::getenv("RRL_TEST_RDMA_HOST");
    const char* port_text = std::getenv("RRL_TEST_RDMA_PORT");
    if (host == nullptr || port_text == nullptr) {
        std::cout << "skip: set RRL_TEST_RDMA_HOST and RRL_TEST_RDMA_PORT to run\n";
        return 77;
    }

    RemoteLockManager locks;
    locks.set_connection_count(4);
    if (!locks.connect(host, static_cast<std::uint16_t>(std::stoul(port_text)))) {
        std::cerr << "FAIL: configured lock server is unreachable\n";
        return 1;
    }

    const KeyRangeEnt point = KeyRangeEnt::point(1000);
    check(locks.lock(1, LockMode::S, point, LockKind::NextKey) == RCOK, "first lock succeeds");
    check(locks.get_lock_count(1) == 1, "one lock recorded");
    check(locks.lock(1, LockMode::S, point, LockKind::NextKey) == RCOK, "repeated lock is idempotent");
    check(locks.get_lock_count(1) == 1, "repeated lock does not duplicate");

    check(locks.lock(2, LockMode::X, point, LockKind::NextKey) == AbortConflict,
          "conflicting lock is refused");

    check(locks.unlock(1, point, LockKind::NextKey) == RCOK, "unlock succeeds");
    check(locks.get_lock_count(1) == 0, "the released transaction holds nothing");
    // The cache entry must be gone: a transaction that lost its last lock must
    // not keep walking shards, and a fresh lock has to work end to end.
    check(locks.lock(1, LockMode::S, point, LockKind::NextKey) == RCOK, "relock after unlock");
    check(locks.unlock_all(1) == RCOK, "unlock_all after relock");
    check(locks.get_lock_count(1) == 0, "unlock_all clears the transaction");

    // The conflicting transaction can proceed once the first one let go.
    check(locks.lock(2, LockMode::X, point, LockKind::NextKey) == RCOK, "lock after the conflict cleared");
    check(locks.unlock_all(2) == RCOK, "unlock_all of the exclusive holder");

    // A range spanning several shards exercises the batched release path.
    const KeyRangeEnt wide = KeyRangeEnt::closed(0, 4095);
    check(locks.lock(3, LockMode::S, wide, LockKind::NextKey) == RCOK, "multi-shard lock");
    check(locks.get_lock_count(3) == 1, "multi-shard lock counted once");
    check(locks.unlock_all(3) == RCOK, "multi-shard release");
    check(locks.get_lock_count(3) == 0, "multi-shard release clears the transaction");

    // Two ranges in different shards: releasing one must leave the other, and
    // the shard set must shrink accordingly.
    const KeyRangeEnt low = KeyRangeEnt::point(7);
    const KeyRangeEnt high = KeyRangeEnt::point(30000);
    check(locks.lock(4, LockMode::S, low, LockKind::NextKey) == RCOK, "low range lock");
    check(locks.lock(4, LockMode::S, high, LockKind::NextKey) == RCOK, "high range lock");
    check(locks.get_lock_count(4) == 2, "two ranges recorded");
    check(locks.unlock(4, low, LockKind::NextKey) == RCOK, "release one range");
    check(locks.get_lock_count(4) == 1, "one range left");
    check(locks.unlock_all(4) == RCOK, "release the remaining range");
    check(locks.get_lock_count(4) == 0, "nothing left after unlock_all");

    // Concurrent transactions: every one must end up holding nothing, which is
    // also what proves no gate was stranded by a failed acquisition.
    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kIterations = 100;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    std::vector<int> thread_failures(kThreads, 0);
    for (std::size_t worker = 0; worker < kThreads; ++worker) {
        workers.emplace_back([&, worker] {
            for (std::size_t i = 0; i < kIterations; ++i) {
                const txnid_t txn = 100 + worker * kIterations + i;
                const KeyRangeEnt range = KeyRangeEnt::point(50000 + txn);
                const RC rc = locks.lock(txn, LockMode::X, range, LockKind::NextKey);
                // AbortGate and AbortConflict are legal outcomes under
                // contention; a transport error is not.
                if (rc != RCOK && rc != AbortGate && rc != AbortConflict) ++thread_failures[worker];
                if (rc == RCOK && locks.unlock_all(txn) != RCOK) ++thread_failures[worker];
            }
        });
    }
    for (auto& worker : workers) worker.join();
    for (std::size_t worker = 0; worker < kThreads; ++worker)
        check(thread_failures[worker] == 0, "concurrent worker saw no hard failure");

    const auto diagnostics = locks.diagnostics();
    check(diagnostics.gate_attempts > 0, "gate attempts were recorded");
    check(diagnostics.gate_transport_errors == 0, "no gate failure came from the transport");
    check(locks.rdma_timeouts() == 0, "no RDMA timeout");
    check(locks.rdma_completion_errors() == 0, "no RDMA completion error");
    const RdmaFailureInfo snapshot = locks.last_failure();
    check(snapshot.opcode == 0, "healthy run has no failure snapshot");

    if (failures != 0) return 1;
    std::cout << "rdma lock loopback: OK gate_attempts=" << diagnostics.gate_attempts
              << " retries=" << diagnostics.gate_retries
              << " aborts=" << diagnostics.gate_aborts << "\n";
    return 0;
}
