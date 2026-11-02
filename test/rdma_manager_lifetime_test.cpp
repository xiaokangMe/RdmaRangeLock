#include "lock/remote_lock_manager.h"
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <new>
#include <thread>
using namespace rrl;
namespace {
void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; std::exit(1); }
}
}
int main() {
    const char* host = std::getenv("RRL_TEST_RDMA_HOST");
    const char* port = std::getenv("RRL_TEST_RDMA_PORT");
    if (!host || !port) return 77;
    alignas(RemoteLockManager) std::byte storage[sizeof(RemoteLockManager)];
    auto* manager = new (storage) RemoteLockManager;
    manager->set_connection_count(2);
    check(manager->connect(host, static_cast<std::uint16_t>(std::stoul(port))), "old manager connect");
    // Main claims slot 0. The persistent worker must cache slot 1.
    check(manager->lock(1, LockMode::S, KeyRangeEnt::point(100)) == RCOK &&
          manager->unlock_all(1) == RCOK, "main claims old slot zero");
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false, reconstructed = false;
    std::thread worker([&] {
        check(manager->lock(2, LockMode::S, KeyRangeEnt::point(101)) == RCOK &&
              manager->unlock_all(2) == RCOK, "worker claims old slot one");
        std::unique_lock<std::mutex> guard(mutex);
        ready = true; cv.notify_all();
        cv.wait(guard, [&] { return reconstructed; });
        guard.unlock();
        check(manager->lock(3, LockMode::X, KeyRangeEnt::point(102)) == RCOK &&
              manager->unlock_all(3) == RCOK, "worker rebinds after address reuse");
    });
    {
        std::unique_lock<std::mutex> guard(mutex);
        cv.wait(guard, [&] { return ready; });
    }
    manager->~RemoteLockManager();
    manager = new (storage) RemoteLockManager;
    manager->set_connection_count(1);
    check(manager->connect(host, static_cast<std::uint16_t>(std::stoul(port))), "new single-slot manager connect");
    {
        std::lock_guard<std::mutex> guard(mutex);
        reconstructed = true;
    }
    cv.notify_all(); worker.join();
    manager->~RemoteLockManager();
    std::cout << "manager address reuse: OK\n";
}
