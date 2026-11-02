#include "lock/remote_lock_manager.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using namespace rrl;

namespace {
[[noreturn]] void recovery_failed(std::uint64_t owner, const char* reason) {
    std::cerr << "RDMA recovery failed closed: owner=" << owner << " " << reason << std::endl;
    std::_Exit(2);
}

void clear_owner_state(std::uint16_t port, std::uint64_t owner, RdmaOpTag failed_tag) {
    if (owner == 0) return; // Unowned probes/recovery connections never publish locks.
    // Use NIC atomics for both ordinary clients and recovery. CPU atomics are
    // not a portable mutual exclusion mechanism against remote NIC atomics.
    RdmaConnection recovery;
    if (!recovery.connect_client("127.0.0.1", port)) recovery_failed(owner, "connect");
    const auto remote = recovery.remote_memory();
    const auto token = owner | (std::uint64_t{1} << 63);
    std::vector<RemoteLockSlot> slots(kRemoteLockSlotsPerShard);
    for (std::size_t shard = 0; shard < kRemoteLockShards; ++shard) {
        const auto base = remote.address + offsetof(RemoteLockRegion, shards) +
                          shard * sizeof(RemoteLockShard);
        const auto gate = base + offsetof(RemoteLockShard, gate);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            std::uint64_t previous = 0;
            if (!recovery.compare_swap(gate, remote.rkey, 0, token, previous))
                recovery_failed(owner, "acquire CAS");
            if (previous == 0) break;
            if (previous == owner) {
                // A torn slot WRITE is not a recoverable transaction. Never
                // reopen its shard without a journal/publication protocol.
                if (failed_tag == RdmaOpTag::None || failed_tag == RdmaOpTag::StoreSlot)
                    recovery_failed(owner, "uncertain mutation while holding gate");
                if (!recovery.compare_swap(gate, remote.rkey, owner, token, previous) || previous != owner)
                    recovery_failed(owner, "takeover CAS");
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                recovery_failed(owner, "gate deadline");
            std::this_thread::yield();
        }
        std::uint64_t count = 0;
        if (!recovery.read(base + offsetof(RemoteLockShard, active_slots), remote.rkey,
                           &count, sizeof(count)) || count > kRemoteLockSlotsPerShard)
            recovery_failed(owner, "count");
        if (count && !recovery.read(base + offsetof(RemoteLockShard, slots), remote.rkey,
                                    slots.data(), count * sizeof(RemoteLockSlot)))
            recovery_failed(owner, "slots");
        std::uint64_t kept = 0;
        for (std::uint64_t i = 0; i < count; ++i)
            if (slots[i].owner != owner) slots[kept++] = slots[i];
        if (kept != count) {
            if (kept && !recovery.write(base + offsetof(RemoteLockShard, slots), remote.rkey,
                                        slots.data(), kept * sizeof(RemoteLockSlot)))
                recovery_failed(owner, "compact");
            if (!recovery.write(base + offsetof(RemoteLockShard, active_slots), remote.rkey,
                                &kept, sizeof(kept))) recovery_failed(owner, "publish count");
        }
        std::uint64_t previous = 0;
        if (!recovery.compare_swap(gate, remote.rkey, token, 0, previous) || previous != token)
            recovery_failed(owner, "release CAS");
    }
}
}

int main(int argc, char** argv) {
    const std::uint16_t port = argc > 1 ? static_cast<std::uint16_t>(std::stoul(argv[1])) : 19876;
    auto region_owner = std::unique_ptr<RemoteLockRegion>(new RemoteLockRegion());
    RemoteLockRegion& region = *region_owner;
    RdmaListener listener(port);
    if (!listener.valid()) { std::cerr << "RDMA control listen failed\n"; return 2; }
    std::cout << "RDMA lock server listening on port " << port << std::endl;
    for (;;) {
        auto* connection = new RdmaConnection();
        if (!connection->accept_server(listener, &region, sizeof(region))) {
            delete connection;
            std::cerr << "RDMA lock server accept failed\n";
            return 2;
        }
        std::thread([connection, port] {
            connection->wait_for_disconnect();
            const auto owner = connection->peer_owner_token();
            const auto tag = connection->peer_failure_tag();
            // Destroy the server QP and invalidate its MR before any recovery
            // can acquire gates or read slots written through that connection.
            delete connection;
            clear_owner_state(port, owner, tag);
        }).detach();
    }
}
