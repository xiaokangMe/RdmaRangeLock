#include "lock/remote_lock_manager.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>
#include <thread>
#include <unistd.h>

using namespace rrl;

// Exercise real verbs completions as well as validation before any WR is posted.
// The last shard's inactive slots are scratch space; active_slots stays zero.
bool transport_failure(int line) {
    std::cerr << "transport validation failed at line " << line << "\n";
    return false;
}

bool check_batch_transport(const std::string& host, std::uint16_t port) {
    RdmaConnection connection;
    if (!connection.connect_client(host, port)) return transport_failure(__LINE__);
    const auto remote = connection.remote_memory();
    if (remote.length < sizeof(RemoteLockRegion)) return transport_failure(__LINE__);
    const auto base = remote.address + offsetof(RemoteLockRegion, shards) +
                      (kRemoteLockShards - 1) * sizeof(RemoteLockShard) +
                      offsetof(RemoteLockShard, slots);
    std::vector<std::uint64_t> values(128), results(128), previous(128);
    std::vector<RdmaWriteRequest> writes;
    std::vector<RdmaReadRequest> reads;
    std::vector<RdmaCompareSwapRequest> swaps;
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = i + 17;
        const auto address = base + i * sizeof(std::uint64_t);
        writes.push_back({address, remote.rkey, &values[i], sizeof(values[i])});
        reads.push_back({address, remote.rkey, &results[i], sizeof(results[i])});
        // Alternating success/failure verifies every CAS result, not just the tail.
        swaps.push_back({address, remote.rkey, i % 2 ? 0 : values[i],
                         values[i] + 100, &previous[i]});
    }
    if (!connection.read_batch({}) || !connection.write_batch({}) ||
        !connection.compare_swap_batch({})) return transport_failure(__LINE__);
    if (!connection.write_batch(writes) || !connection.read_batch(reads) ||
        results != values) return transport_failure(__LINE__);

    const auto huge = std::numeric_limits<std::size_t>::max();
    if (connection.read_batch({{base, remote.rkey, &results[0], huge}}) ||
        connection.write_batch({writes[0], {base, remote.rkey, &values[0], huge}}) ||
        connection.read_batch({{base, remote.rkey, nullptr, 8}}) ||
        connection.write_batch({{base, remote.rkey, nullptr, 8}}) ||
        connection.compare_swap_batch({{base, remote.rkey, 0, 1, nullptr}}) ||
        connection.read_batch({reads[0], {0, remote.rkey, &results[0], 8}}) ||
        connection.write_batch({writes[0], {0, remote.rkey, &values[0], 8}}) ||
        connection.compare_swap_batch({swaps[0], {base + 1, remote.rkey, 0, 1, &previous[0]}})) return transport_failure(__LINE__);
    std::vector<std::uint8_t> scratch((1u << 20) + 1);
    if (!connection.read(remote.address, remote.rkey, scratch.data(), 1u << 20)) return transport_failure(__LINE__);
    const auto after_read = connection.operation_count();
    if (connection.read(remote.address, remote.rkey, scratch.data(), scratch.size()) ||
        connection.write(remote.address, remote.rkey, scratch.data(), scratch.size()) ||
        connection.read_batch({{base, remote.rkey, scratch.data(), 1u << 19},
                               {base, remote.rkey, scratch.data(), (1u << 19) + 1}}) ||
        connection.operation_count() != after_read) return transport_failure(__LINE__);
    writes.push_back(writes.front());
    reads.push_back(reads.front());
    swaps.push_back(swaps.front());
    if (connection.write_batch(writes) || connection.read_batch(reads) ||
        connection.compare_swap_batch(swaps) ||
        connection.operation_count() != after_read) return transport_failure(__LINE__);
    swaps.pop_back();
    reads.pop_back();
    if (!connection.compare_swap_batch(swaps) || previous != values ||
        !connection.read_batch(reads)) return transport_failure(__LINE__);
    for (std::size_t i = 0; i < values.size(); ++i)
        if (results[i] != values[i] + (i % 2 ? 0 : 100)) return transport_failure(__LINE__);
    return connection.timeout_count() == 0 && connection.completion_error_count() == 0;
}

int main(int argc, char** argv) {
    const char* env_host = std::getenv("RRL_TEST_RDMA_HOST");
    const char* env_port = std::getenv("RRL_TEST_RDMA_PORT");
    if (argc == 1 && (!env_host || !env_port)) return 77;
    const std::string host = argc > 1 ? argv[1] : env_host;
    const std::uint16_t port = static_cast<std::uint16_t>(std::stoul(argc > 2 ? argv[2] : env_port ? env_port : "19876"));
    if (!check_batch_transport(host, port)) {
        std::cerr << "RDMA batch transport validation failed\n";
        return 8;
    }
    RemoteLockManager locks;
    if (!locks.connect(host, port)) {
        std::cerr << "RDMA lock client connection failed\n";
        return 2;
    }
    if (locks.lock(7, LockMode::S, KeyRangeEnt::open(10, 20), LockKind::NextKey) != RCOK) return 3;
    if (locks.lock(7, LockMode::S, KeyRangeEnt::point(20), LockKind::NextKey) != RCOK) return 4;
    if (locks.lock(8, LockMode::X, KeyRangeEnt::point(15), LockKind::NextKey) != Abort) return 5;
    if (locks.get_lock_count(7) != 2) return 6;
    locks.unlock_all(7);
    if (locks.lock(8, LockMode::X, KeyRangeEnt::point(15), LockKind::NextKey) != RCOK) return 7;
    if (argc > 3 && std::string(argv[3]) == "crash") _exit(0);
    locks.unlock_all(8);
    // An all-shard request must release every successful CAS if one gate is busy.
    {
        RdmaConnection probe;
        if (!probe.connect_client(host, port)) return 9;
        const auto remote = probe.remote_memory();
        const auto shards = remote.address + offsetof(RemoteLockRegion, shards);
        const auto busy = shards + (kRemoteLockShards / 2) * sizeof(RemoteLockShard) +
                          offsetof(RemoteLockShard, gate);
        std::uint64_t token = 1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            std::uint64_t previous = 0;
            if (!probe.compare_swap(busy, remote.rkey, 0, token, previous)) return 10;
            if (previous == 0) break;
            if (std::chrono::steady_clock::now() >= deadline) return 10;
            std::this_thread::yield();
        }
        const auto rc = locks.lock(9, LockMode::S, KeyRangeEnt::unbounded());
        std::uint64_t previous = 0;
        if (!probe.compare_swap(busy, remote.rkey, token, 0, previous) || previous != token) return 11;
        if (rc != AbortGate) return 12;
        for (std::size_t shard = 0; shard < kRemoteLockShards; ++shard) {
            std::uint64_t gate = 1;
            if (!probe.read(shards + shard * sizeof(RemoteLockShard) +
                                offsetof(RemoteLockShard, gate),
                            remote.rkey, &gate, sizeof(gate)) || gate != 0) return 13;
        }
    }
    // Exercise batched counts, slot reads, upgrades and publication over all shards.
    const auto all = KeyRangeEnt::unbounded();
    if (locks.lock(10, LockMode::S, all) != RCOK ||
        locks.lock(11, LockMode::S, all) != RCOK ||
        locks.lock(10, LockMode::X, all) != Abort ||
        locks.unlock_all(11) != RCOK ||
        locks.lock(10, LockMode::X, all) != RCOK ||
        locks.get_lock_count() != 1 || locks.unlock_all(10) != RCOK ||
        locks.get_lock_count() != 0) return 14;
    if (locks.rdma_timeouts() != 0 || locks.rdma_completion_errors() != 0) return 15;
    // A next-key range read can publish several intervals with one gate/read
    // round. Keep the ranges on different shards to exercise the union path.
    auto* batch = dynamic_cast<IRangeLockBatch*>(&locks);
    if (!batch) return 16;
    const std::vector<KeyRangeEnt> grouped{KeyRangeEnt::point(30), KeyRangeEnt::open(70, 90)};
    if (batch->lock_batch(12, LockMode::S, grouped, LockKind::NextKey) != RCOK ||
        locks.get_lock_count(12) != grouped.size() ||
        locks.lock(13, LockMode::X, KeyRangeEnt::point(80), LockKind::NextKey) != AbortConflict ||
        locks.unlock_all(12) != RCOK ||
        locks.lock(13, LockMode::X, KeyRangeEnt::point(80), LockKind::NextKey) != RCOK ||
        locks.unlock_all(13) != RCOK) return 17;
    // Exact-range uses one physical interval per logical query. Its kind is
    // carried through the remote slot so it cannot alias predicate/next-key
    // entries during upgrade or release.
    const auto precise = KeyRangeEnt::half_open(100, 120);
    if (locks.lock(20, LockMode::S, precise, LockKind::ExactRange) != RCOK ||
        locks.lock(21, LockMode::X, KeyRangeEnt::point(110), LockKind::ExactRange) != AbortConflict ||
        locks.lock(21, LockMode::X, KeyRangeEnt::point(120), LockKind::ExactRange) != RCOK ||
        locks.unlock_all(20) != RCOK || locks.unlock_all(21) != RCOK) return 18;
    std::cout << "RDMA batch transport, grouped next-key, exact-range publication and remote lock: OK\n";
    return 0;
}
