#include "txn/coroutine.h"
#include "common/execution_context.h"
#include "common/fifo_mutex.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
int main() {
    const auto thread=std::this_thread::get_id();
    std::vector<int> order;
    rrl::FifoMutex mutex;
    rrl::CoroutineScheduler scheduler(4,65536);
    scheduler.run([&](std::size_t id) {
        if(std::this_thread::get_id()!=thread)std::abort();
        int marker=static_cast<int>(id);
        rrl::execution_index_session()=&marker;
        const auto identity=rrl::execution_context_id();
        std::lock_guard<rrl::FifoMutex> guard(mutex);
        // Yield WITH the mutex: later routines must yield rather than block
        // this sole OS worker. All four stack/local buffer identities survive.
        for(int i=0;i<20;++i) {
            rrl::cooperative_yield();
            if(rrl::execution_context_id()!=identity ||
               rrl::execution_index_session()!=&marker || marker!=static_cast<int>(id))std::abort();
        }
        order.push_back(static_cast<int>(id));
        rrl::execution_index_session()=nullptr;
    });
    if(order!=std::vector<int>({0,1,2,3}) || scheduler.switches()<80 ||
       rrl::coroutine_active() || rrl::execution_index_session())return 1;
    bool unwound=false,peer_finished=false;
    rrl::CoroutineScheduler errors(2,65536);
    try {
        errors.run([&](std::size_t id) {
            if(id==0) {
                struct Cleanup {bool& flag;~Cleanup(){flag=true;}} cleanup{unwound};
                rrl::cooperative_yield();throw std::runtime_error("expected");
            }
            rrl::cooperative_yield();peer_finished=true;
        });
        return 2;
    } catch(const std::runtime_error&) {}
    if(!unwound || !peer_finished || rrl::coroutine_active())return 3;
    std::cout<<"Single-worker stackful FIFO progress, isolated contexts and exception cleanup: OK\n";
}
