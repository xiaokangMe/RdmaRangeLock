#include "lock/remote_lock_manager.h"

#include <cstdint>
#include <iostream>
#include <thread>

using namespace rrl;

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
        std::thread([connection] {
            connection->wait_for_disconnect();
            delete connection;
        }).detach();
    }
}