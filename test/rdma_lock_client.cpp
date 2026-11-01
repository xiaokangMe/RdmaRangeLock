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
bool check_batch_transport(const std::string& host, std::uint16_t port) {
    RdmaConnection connection;
    if (!connection.connect_client(host, port)) return false;
    const auto remote = connection.remote_memory();
    if (remote.length < sizeof(RemoteLockRegion)) return false;
    const auto base = remote.address + offsetof(RemoteLockRegion, shards) +
                      (kRemoteLockShards - 1) * sizeof(RemoteLockShard) +
                      offsetof(RemoteLockShard, slots);
    std::vector<std::uint64_t> values(64), results(64), previous(64);
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
        !connection.compare_swap_batch({})) return false;
    if (!connection.write_batch(writes) || !connection.read_batch(reads) ||
        results != values) return false;
    const auto operations = connection.operation_count();
    const auto huge = std::numeric_limits<std::size_t>::max();
    if (connection.read_batch({{base, remote.rkey, &results[0], huge}}) ||
        connection.write_batch({writes[0], {base, remote.rkey, &values[0], huge}}) ||
        connection.read_batch({{base, remote.rkey, nullptr, 8}}) ||
        connection.write_batch({{base, remote.rkey, nullptr, 8}}) ||
        connection.compare_swap_batch({{base, remote.rkey, 0, 1, nullptr}})) return false;
    writes.push_back(writes.front());
    reads.push_back(reads.front());
    swaps.push_back(swaps.front());
    if (connection.write_batch(writes) || connection.read_batch(reads) ||
        connection.compare_swap_batch(swaps) ||
        connection.operation_count() != operations) return false;
    swaps.pop_back();
    reads.pop_back();
    if (!connection.compare_swap_batch(swaps) || previous != values ||
        !connection.read_batch(reads)) return false;
    for (std::size_t i = 0; i < values.size(); ++i)
        if (results[i] != values[i] + (i % 2 ? 0 : 100)) return false;
    return connection.timeout_count() == 0 && connection.completion_error_count() == 0;
}

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const std::uint16_t port = argc > 2 ? static_cast<std::uint16_t>(std::stoul(argv[2])) : 19876;
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
        if (!probe.write(busy, remote.rkey, &token, sizeof(token))) return 10;
        const auto rc = locks.lock(9, LockMode::S, KeyRangeEnt::unbounded());
        token = 0;
        if (!probe.write(busy, remote.rkey, &token, sizeof(token))) return 11;
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
    std::cout << "RDMA batch transport and remote next-key lock: OK\n";
    return 0;
}
