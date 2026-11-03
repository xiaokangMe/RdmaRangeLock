#include "hierarchy_contract.h"
#include "lock/remote_lock_manager.h"
#include <condition_variable>
#include <thread>
class Barrier {
public:
    explicit Barrier(std::size_t n): n_(n) {}
    void wait() {
        std::unique_lock<std::mutex> guard(m_);
        const auto generation=generation_;
        if (++arrived_==n_) { arrived_=0; ++generation_; cv_.notify_all(); }
        else cv_.wait(guard,[&]{return generation_!=generation;});
    }
private:
    std::mutex m_; std::condition_variable cv_;
    std::size_t n_,arrived_{0},generation_{0};
};
int main() {
    using namespace rrl;
    using hierarchy_test::check;
    const char* host=std::getenv("RRL_TEST_RDMA_HOST");
    const char* port=std::getenv("RRL_TEST_RDMA_PORT");
    if (!host || !port) return 77;
    RemoteLockManager m;
    m.set_bucket_width(256); m.set_connection_count(2);
    check(m.connect(host,static_cast<std::uint16_t>(std::stoul(port))),"connect");
    hierarchy_test::contract(m);
    constexpr std::size_t count=8, rounds=16;
    Barrier barrier(count);
    std::vector<std::thread> workers;
    std::array<unsigned,count> committed{};
    for (std::size_t worker=0;worker<count;++worker) workers.emplace_back([&,worker] {
        const auto plan=hierarchy_test::point_plan(100+worker,true);
        for (std::size_t round=0;round<rounds;++round) {
            const txnid_t tid=1000+worker*rounds+round;
            barrier.wait();
            check(hierarchy_test::acquire(m,tid,plan)==RCOK,"same-bucket disjoint records make progress with two QPs");
            barrier.wait(); // every transaction holds its record simultaneously
            ++committed[worker];
            check(m.unlock_all(tid)==RCOK,"each queued release completes");
            barrier.wait();
        }
    });
    for(auto& worker:workers) worker.join();
    for(auto n:committed) check(n==rounds,"every worker commits all disjoint transactions");
    check(m.get_lock_count()==0,"remote lock table is empty");
    check(!m.rdma_timeouts() && !m.rdma_completion_errors() &&
          !m.diagnostics().gate_transport_errors,"no transport failure");
    std::cout<<"RDMA hierarchy: OK, every worker committed "<<rounds<<", final locks=0\n";
}
