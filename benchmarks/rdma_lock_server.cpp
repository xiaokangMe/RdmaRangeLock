#include "lock/remote_lock_manager.h"

#include <cstdint>
#include <iostream>
#include <thread>

using namespace rrl;

namespace {
void clear_owner_state(RemoteLockRegion& region, std::uint64_t owner) {
    if (owner == 0) return;
    const std::uint64_t count = __atomic_load_n(&region.active_slots, __ATOMIC_ACQUIRE);
    std::uint64_t write_index = 0;
    for (std::uint64_t i = 0; i < count && i < kRemoteLockSlots; ++i) {
        if (region.slots[i].owner == owner) continue;
        if (write_index != i) region.slots[write_index] = region.slots[i];
        ++write_index;
    }
    for (std::uint64_t i = write_index; i < count && i < kRemoteLockSlots; ++i)
        region.slots[i] = RemoteLockSlot{};
    __atomic_store_n(&region.active_slots, write_index, __ATOMIC_RELEASE);
    std::uint64_t expected = owner;
    __atomic_compare_exchange_n(&region.gate, &expected, std::uint64_t{0}, false,
                                __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}
}

int main(int argc, char** argv) {
    const std::uint16_t port = argc > 1 ? static_cast<std::uint16_t>(std::stoul(argv[1])) : 19876;
    RemoteLockRegion region{};
    std::cout << "RDMA lock server listening on port " << port << std::endl;
    for (;;) {
        auto* connection = new RdmaConnection();
        if (!connection->accept_server(port, &region, sizeof(region))) {
            delete connection;
            std::cerr << "RDMA lock server accept failed\n";
            return 2;
        }
        std::cout << "RDMA lock client connected" << std::endl;
        std::thread([connection, &region] {
            connection->wait_for_disconnect();
            clear_owner_state(region, connection->peer_owner_token());
            delete connection;
        }).detach();
    }
}
