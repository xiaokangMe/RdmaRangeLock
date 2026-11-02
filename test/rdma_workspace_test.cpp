// Real-QP regression: allocation boundaries, reusable snapshots, exception cleanup.
#include "lock/remote_lock_manager.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
thread_local bool in_gate = false;
thread_local std::size_t gate_allocations = 0;
std::size_t gate_visits = 0;
void gate_state(bool held) { in_gate = held; if (held) ++gate_visits; }
void throw_after_gate() { throw std::bad_alloc(); }
void check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; std::exit(1); }
}
}
void* operator new(std::size_t size) {
    if (in_gate) ++gate_allocations;
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }

namespace rrl {
struct RemoteLockManagerTestPeer {
    static void observe(RemoteLockManager& m) { m.gate_state_test_hook_ = gate_state; }
    static void inject(RemoteLockManager& m) { m.after_gates_test_hook_ = throw_after_gate; }
};
}
using namespace rrl;
int main() {
    const char* host = std::getenv("RRL_TEST_RDMA_HOST");
    const char* text = std::getenv("RRL_TEST_RDMA_PORT");
    if (!host || !text) return 77;
    const auto port = static_cast<std::uint16_t>(std::stoul(text));
    RemoteLockManager locks;
    check(locks.connect(host, port), "connect");
    RemoteLockManagerTestPeer::observe(locks);
    // More than 128 compaction WRs, stable payload pointers across chunks.
    for (std::size_t i = 0; i < 140; ++i) {
        const auto range = KeyRangeEnt::closed(i, 4095 + i);
        check(locks.lock(30, LockMode::S, range) == RCOK, "wide lock");
        check(locks.lock(31, LockMode::S, range) == RCOK, "survivor lock");
    }
    check(locks.get_lock_count() == 280, "global dedup");
    check(locks.unlock_all(30) == RCOK, "multi-chunk compaction");
    check(locks.get_lock_count() == 140, "survivors preserved");
    check(locks.lock(32, LockMode::X, KeyRangeEnt::point(200)) == AbortConflict, "survivor conflicts");
    check(locks.unlock_all(31) == RCOK, "survivor release");
    // Reusing a workspace with a smaller shard set must discard stale tables.
    check(locks.lock(33, LockMode::X, KeyRangeEnt::point(7)) == RCOK, "one shard after all shards");
    check(locks.lock(34, LockMode::X, KeyRangeEnt::point(99999)) == RCOK, "different one shard");
    check(locks.get_lock_count() == 2, "changing shard sets");
    check(locks.unlock_all(33) == RCOK && locks.unlock_all(34) == RCOK, "point release");
    check(locks.get_lock_count() == 0, "empty after release");
    check(gate_visits > 280 && gate_allocations == 0 && !in_gate, "zero C++ new allocations while gates held");

    RdmaConnection observer;
    check(observer.connect_client(host, port), "observer connect");
    const auto memory = observer.remote_memory();
    for (int operation = 0; operation < 3; ++operation) {
        RemoteLockManager failing;
        check(failing.connect(host, port), "exception manager connect");
        if (operation == 1)
            check(failing.lock(50, LockMode::S, KeyRangeEnt::closed(0, 4095)) == RCOK, "release setup");
        RemoteLockManagerTestPeer::inject(failing);
        if (operation == 0)
            check(failing.lock(50, LockMode::S, KeyRangeEnt::closed(0, 4095)) == ERROR, "lock exception reports error");
        else if (operation == 1)
            check(failing.unlock_all(50) == ERROR, "release exception reports error");
        else
            check(failing.get_lock_count() == 0 && failing.diagnostics().count_errors == 1,
                  "count exception distinguishable from empty");
        check(!failing.connected() && failing.diagnostics().local_errors == 1, "exception invalidates manager");
        check(failing.lock(50, LockMode::S, KeyRangeEnt::point(1)) == ERROR, "invalid manager cannot hit cache");
        for (std::size_t shard = 0; shard < kRemoteLockShards; ++shard) {
            std::uint64_t gate = 1;
            check(observer.read(memory.address + offsetof(RemoteLockRegion, shards) +
                  shard * sizeof(RemoteLockShard) + offsetof(RemoteLockShard, gate),
                  memory.rkey, &gate, sizeof(gate)) && gate == 0, "all exception gates released");
        }
    }
    std::cout << "RDMA workspace: OK, gate allocations=" << gate_allocations << "\n";
}
