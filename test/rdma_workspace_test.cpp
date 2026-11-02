// Real-QP regression: allocation boundaries, reusable snapshots, exception cleanup.
#include "lock/remote_lock_manager.h"
#include <array>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
thread_local bool in_gate = false;
thread_local std::size_t gate_allocations = 0;
std::size_t gate_visits = 0;
std::mutex pause_mutex;
std::condition_variable paused_cv;
bool paused = false, resume = false;
void before_cache() {
    std::unique_lock<std::mutex> lock(pause_mutex);
    paused = true;
    paused_cv.notify_all();
    paused_cv.wait(lock, [] { return resume; });
}
void gate_state(bool held) { in_gate = held; if (held) ++gate_visits; }
void throw_after_gate() { throw std::bad_alloc(); }
void check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; std::exit(1); }
}
rrl::RdmaConnection* exception_observer = nullptr;
void verify_exception_gates(bool held) {
    if (held) return;
    const auto memory = exception_observer->remote_memory();
    for (std::size_t shard = 0; shard < rrl::kRemoteLockShards; ++shard) {
        std::uint64_t gate = 1;
        check(exception_observer->read(memory.address + offsetof(rrl::RemoteLockRegion, shards) +
              shard * sizeof(rrl::RemoteLockShard) + offsetof(rrl::RemoteLockShard, gate),
              memory.rkey, &gate, sizeof(gate)) && gate == 0,
              "exception gates released before peer recovery starts");
    }
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

void* operator new(std::size_t size, std::align_val_t alignment) {
    if (in_gate) ++gate_allocations;
    void* result = nullptr;
    if (posix_memalign(&result, static_cast<std::size_t>(alignment), size ? size : 1) == 0)
        return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void* ptr, std::align_val_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { std::free(ptr); }

namespace rrl {
struct RemoteLockManagerTestPeer {
    static void observe(RemoteLockManager& m) { m.gate_state_test_hook_ = gate_state; }
    static void inject(RemoteLockManager& m, bool published = false) {
        m.gate_state_test_hook_ = verify_exception_gates;
        if (published) m.before_cache_test_hook_ = throw_after_gate;
        else m.after_gates_test_hook_ = throw_after_gate;
    }
    static void pause_cache(RemoteLockManager& m) { m.before_cache_test_hook_ = before_cache; }
    static bool txn_available(RemoteLockManager& m, txnid_t txn) {
        auto& mutex = m.txn_operation_mutexes_[txn % kRemoteTxnCacheShards];
        if (!mutex.try_lock()) return false;
        mutex.unlock(); return true;
    }
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

    // A different QP must not release using a shard set that predates B.
    RemoteLockManager ordered;
    ordered.set_connection_count(3);
    check(ordered.connect(host, port), "transaction-order connect");
    const auto a = KeyRangeEnt::point(7), b = KeyRangeEnt::point(200);
    check(ordered.lock(90, LockMode::S, a) == RCOK, "order setup A");
    RemoteLockManagerTestPeer::pause_cache(ordered);
    RC publish_rc = ERROR, release_rc = ERROR;
    std::thread publish([&] { publish_rc = ordered.lock(90, LockMode::S, b); });
    {
        std::unique_lock<std::mutex> lock(pause_mutex);
        paused_cv.wait(lock, [] { return paused; });
    }
    check(!RemoteLockManagerTestPeer::txn_available(ordered, 90),
          "transaction operation protected after gate release");
    std::thread release([&] { release_rc = ordered.unlock_all(90); });
    {
        std::lock_guard<std::mutex> lock(pause_mutex);
        resume = true;
    }
    paused_cv.notify_all();
    publish.join(); release.join();
    check(publish_rc == RCOK && release_rc == RCOK && ordered.get_lock_count(90) == 0 &&
          ordered.get_lock_count() == 0, "cross-QP release sees both published shards");
    check(locks.lock(91, LockMode::X, b) == RCOK && locks.unlock_all(91) == RCOK,
          "other transaction sees no leaked B");

    RdmaConnection observer;
    check(observer.connect_client(host, port), "observer connect");
    const auto memory = observer.remote_memory();
    exception_observer = &observer;
    for (int operation = 0; operation < 4; ++operation) {
        RemoteLockManager failing;
        check(failing.connect(host, port), "exception manager connect");
        // A sentinel on every shard makes completion of async cleanup visible.
        check(failing.lock(49, LockMode::S, KeyRangeEnt::unbounded()) == RCOK, "cleanup sentinel");
        if (operation == 1)
            check(failing.lock(50, LockMode::S, KeyRangeEnt::closed(0, 4095)) == RCOK, "release setup");
        RemoteLockManagerTestPeer::inject(failing, operation == 3);
        if (operation == 0 || operation == 3)
            check(failing.lock(50, LockMode::S, KeyRangeEnt::closed(0, 4095)) == ERROR, "lock exception reports error");
        else if (operation == 1)
            check(failing.unlock_all(50) == ERROR, "release exception reports error");
        else
            check(failing.get_lock_count() == 0 && failing.diagnostics().count_errors == 1,
                  "count exception distinguishable from empty");
        check(!failing.connected() && failing.diagnostics().local_errors == 1, "exception invalidates manager");
        check(failing.lock(50, LockMode::S, KeyRangeEnt::point(1)) == ERROR, "invalid manager cannot hit cache");
        const auto last = memory.address + offsetof(RemoteLockRegion, shards) +
                          (kRemoteLockShards - 1) * sizeof(RemoteLockShard);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        // Observe sentinel removal on the final shard, not a fixed sleep.
        // ReleaseGate happens after that count publication; both must finish.
        for (;;) {
            std::uint64_t active = 1, gate = 1;
            check(observer.read(last + offsetof(RemoteLockShard, active_slots), memory.rkey, &active, 8) &&
                  observer.read(last + offsetof(RemoteLockShard, gate), memory.rkey, &gate, 8), "observe cleanup");
            if (active == 0 && gate == 0) break;
            check(std::chrono::steady_clock::now() < deadline, "exception cleanup deadline");
            std::this_thread::yield();
        }
    }
    std::cout << "RDMA workspace: OK, gate allocations=" << gate_allocations << "\n";
}
