#include "common/fifo_mutex.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>
namespace rrl {
struct FifoMutexTestPeer {
    static std::uint64_t admitted(FifoMutex& m) {
        std::lock_guard<std::mutex> guard(m.mutex_); return m.next_;
    }
};
}
int main() {
    rrl::FifoMutex m;
    std::vector<int> order;
    m.lock();
    std::vector<std::thread> workers;
    for (int i=0;i<12;++i) {
        workers.emplace_back([&,i] { std::lock_guard<rrl::FifoMutex> g(m); order.push_back(i); });
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        // Test synchronization only: observe ticket admission before launching
        // a later caller. This is not a retry of an aborted transaction.
        while (rrl::FifoMutexTestPeer::admitted(m) != static_cast<unsigned>(i+2)) {
            if (std::chrono::steady_clock::now() >= deadline) std::exit(1);
            std::this_thread::yield();
        }
        if (m.try_lock()) std::exit(2); // a newcomer must not bypass the queue
    }
    m.unlock();
    for (auto& worker:workers) worker.join();
    for (int i=0;i<12;++i) if(order[i]!=i) std::exit(3);
    if (!m.try_lock()) std::exit(4);
    m.unlock();
    std::cout << "FIFO local-resource admission: OK\n";
}
