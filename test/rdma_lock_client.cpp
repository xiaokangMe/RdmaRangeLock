#include "lock/remote_lock_manager.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace rrl;

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const std::uint16_t port = argc > 2 ? static_cast<std::uint16_t>(std::stoul(argv[2])) : 19876;
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
    locks.unlock_all(8);
    std::cout << "RDMA remote next-key lock: OK\n";
    return 0;
}