#include "lock/remote_lock_manager.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <unistd.h>
using namespace rrl;
namespace {
void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; std::exit(1); }
}
}
int main(int argc, char** argv) {
    const char* host = std::getenv("RRL_TEST_RDMA_HOST");
    const char* port_text = std::getenv("RRL_TEST_RDMA_PORT");
    if (!host || !port_text) return 77;
    const auto port = static_cast<std::uint16_t>(std::stoul(port_text));
    const bool unknown = argc > 1 && std::string(argv[1]) == "unknown";
    const bool unsafe = unknown || (argc > 1 && std::string(argv[1]) == "unsafe");
    // These cases use an isolated server. A bad rkey causes a real failed WC.
    constexpr std::uint64_t owner = 0x123456789ULL;
    RdmaConnection failed;
    check(failed.connect_client(host, port, owner), "failed-owner connect");
    if (argc > 1 && std::string(argv[1]) == "budget") {
        std::cout << "budget owner ready" << std::endl;
        std::cin.get();
        return 0;
    }
    const auto remote = failed.remote_memory();
    const auto base = remote.address + offsetof(RemoteLockRegion, shards);
    const auto gate = base + offsetof(RemoteLockShard, gate);
    if (unsafe) {
        std::uint64_t previous = 0;
        check(failed.compare_swap(gate, remote.rkey, 0, owner, previous) && previous == 0, "unsafe acquire");
        if (unknown) _exit(0); // No phase notice; gate contents are uncertain.
        failed.set_operation_tag(RdmaOpTag::StoreSlot);
        check(!failed.read(gate, remote.rkey ^ 0x80000000u, &previous, 8), "unsafe real WC failure");
        return 0; // Harness must verify server exits nonzero (fail closed).
    }
    RemoteLockManager live;
    check(live.connect(host, port), "live connect");
    check(live.lock(1, LockMode::S, KeyRangeEnt::point(7)) == RCOK, "live lock");
    std::uint64_t previous = 0;
    check(failed.compare_swap(gate, remote.rkey, 0, owner, previous) && previous == 0, "acquire lost-result gate");
    std::uint64_t count = 0;
    check(failed.read(base + offsetof(RemoteLockShard, active_slots), remote.rkey, &count, 8), "read count");
    RemoteLockSlot slot{};
    encode_remote_slot(slot, owner, 999, LockMode::X, LockKind::Predicate, KeyRangeEnt::point(8));
    check(failed.write(base + offsetof(RemoteLockShard, slots) + count * sizeof(slot),
                       remote.rkey, &slot, sizeof(slot)), "publish dead-owner slot");
    ++count;
    check(failed.write(base + offsetof(RemoteLockShard, active_slots), remote.rkey, &count, 8), "publish count");
    // Deliberately keep the failed object alive: detection must not depend on
    // its destructor. The failed READ has no mutation to recover.
    failed.set_operation_tag(RdmaOpTag::LoadSlots);
    check(!failed.read(gate, remote.rkey ^ 0x80000000u, &previous, 8), "real WC error");
    check(!failed.connected() && failed.failure_count() == 1 && failed.completion_error_count() == 1,
          "failure is visible before destruction");
    const auto operations = failed.operation_count();
    check(!failed.compare_swap(gate, remote.rkey, owner, 0, previous) && failed.operation_count() == operations,
          "failed QP cannot issue cleanup");
    RdmaConnection probe;
    check(probe.connect_client(host, port), "probe connect");
    const auto probe_memory = probe.remote_memory();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    // Poll an observable recovery condition, with a deadline; no fixed sleeps.
    for (;;) {
        std::uint64_t held = owner;
        check(probe.read(gate, probe_memory.rkey, &held, 8), "read recovered gate");
        if (held == 0) break;
        check(std::chrono::steady_clock::now() < deadline, "gate recovery deadline");
        std::this_thread::yield();
    }
    check(live.lock(2, LockMode::X, KeyRangeEnt::point(7)) == AbortConflict, "live lock survives recovery");
    check(live.lock(3, LockMode::X, KeyRangeEnt::point(8)) == RCOK, "dead-owner slot removed");
    check(live.unlock_all(1) == RCOK && live.unlock_all(3) == RCOK, "release surviving locks");
    // A failed CAS batch can have acquired its valid prefix even though no
    // results were delivered to the caller. Recover that ownership as well.
    {
        RdmaConnection partial;
        constexpr auto batch_owner = owner + 1;
        check(partial.connect_client(host, port, batch_owner), "partial batch connect");
        const auto memory = partial.remote_memory();
        std::uint64_t results[2]{};
        partial.set_operation_tag(RdmaOpTag::Gate);
        check(!partial.compare_swap_batch({
            {gate, memory.rkey, 0, batch_owner, &results[0]},
            {gate + sizeof(RemoteLockShard), memory.rkey ^ 0x80000000u, 0, batch_owner, &results[1]}}),
            "real failed gate CAS batch");
        check(!partial.connected(), "failed gate batch disconnects immediately");
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            std::uint64_t held = batch_owner;
            check(probe.read(gate, probe_memory.rkey, &held, 8), "read batch gate");
            if (held == 0) break;
            check(std::chrono::steady_clock::now() < end, "batch gate recovery deadline");
            std::this_thread::yield();
        }
    }
    // Concurrent normal operations during many disconnect cleanups, all on the
    // same shard. Every survivor must remain in conflict until explicitly freed.
    std::atomic<bool> bad{false};
    std::thread worker([&] {
        for (std::uint64_t i = 0; i < 100; ++i) {
            const auto rc = live.lock(1000 + i, LockMode::X, KeyRangeEnt::point(20));
            if (rc == RCOK) { if (live.unlock_all(1000 + i) != RCOK) bad = true; }
            else if (rc != AbortGate) bad = true;
        }
    });
    for (int i = 0; i < 8; ++i) {
        auto dead = std::make_unique<RemoteLockManager>();
        check(dead->connect(host, port), "disconnect owner connect");
        check(dead->lock(99, LockMode::S, KeyRangeEnt::point(30 + i)) == RCOK, "disconnect owner lock");
        dead.reset();
    }
    worker.join();
    check(!bad, "normal operations survive simultaneous cleanup");
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        std::uint64_t held = 1, active = 1;
        check(probe.read(gate, probe_memory.rkey, &held, 8) &&
              probe.read(base + offsetof(RemoteLockShard, active_slots), probe_memory.rkey, &active, 8),
              "observe final cleanup");
        if (held == 0 && active == 0) break;
        check(std::chrono::steady_clock::now() < end, "all disconnected owners reclaimed");
        std::this_thread::yield();
    }
    std::cout << "RDMA recovery: OK\n";
}
