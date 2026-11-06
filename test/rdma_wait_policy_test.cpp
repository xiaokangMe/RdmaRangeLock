#include "lock/wait_policy.h"
#include "lock/remote_lock_manager.h"
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>
using namespace rrl;
void check(bool v,const char* m){if(!v){std::cerr<<"FAIL: "<<m<<std::endl;std::exit(1);}}
int main() {
    const char* host=std::getenv("RRL_TEST_RDMA_HOST");
    const char* port=std::getenv("RRL_TEST_RDMA_PORT");
    const char* control=std::getenv("RRL_TEST_COORDINATOR_PORT");
    if(!host || !port || !control)return 77;
    RemoteLockManager pa,pb;
    pa.use_coordinator_transaction_ids();pb.use_coordinator_transaction_ids();
    check(pa.connect(host,static_cast<std::uint16_t>(std::stoul(port))),"first independent QP connect");
    check(pb.connect(host,static_cast<std::uint16_t>(std::stoul(port))),"second independent QP connect");
    PolicyClient a(host,static_cast<std::uint16_t>(std::stoul(control)),8000);
    PolicyClient b(host,static_cast<std::uint16_t>(std::stoul(control)),8000);
    const auto key=KeyRangeEnt::point(12345);
    for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
        auto old=a.begin(policy,0,5000000000ULL),young=b.begin(policy,0,5000000000ULL);
        check(old.result==TxnResult::Ok && young.result==TxnResult::Ok,"global ages from coordinator");
        check(a.acquire(old.id,LockMode::X,key,LockKind::Record).result==TxnResult::Ok,"old metadata grant");
        check(pa.lock(old.id,LockMode::X,key,LockKind::Record)==RCOK,"old publishes via RDMA");
        if(policy!=WaitPolicy::WoundWait) {
            check(b.acquire(young.id,LockMode::S,key,LockKind::Record).result==TxnResult::Conflict,"younger conflicts/dies");
            check(pa.unlock_all(old.id)==RCOK,"physical release precedes metadata release");
            check(a.end(old.id).result==TxnResult::Ok,"old end");
        } else {
            auto waiter=std::async(std::launch::async,[&]{return b.acquire(young.id,LockMode::S,key,LockKind::Record);});
            check(a.prepare_commit(old.id).result==TxnResult::Ok,"young cannot wound old");
            check(pa.unlock_all(old.id)==RCOK,"old physical release");
            check(a.end(old.id).result==TxnResult::Ok,"old end wakes young");
            check(waiter.get().result==TxnResult::Ok,"young waits and wakes across sessions");
            check(pb.lock(young.id,LockMode::S,key,LockKind::Record)==RCOK,"young publishes after wake");
            check(pb.unlock_all(young.id)==RCOK,"young physical release");
        }
        check(b.end(young.id).result==TxnResult::Ok,"young metadata cleanup");
    }
    for(auto policy:{WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
        auto old=a.begin(policy,0,5000000000ULL),young=b.begin(policy,0,5000000000ULL);
        check(b.acquire(young.id,LockMode::X,key,LockKind::Record).result==TxnResult::Ok,"young metadata grant");
        check(pb.lock(young.id,LockMode::X,key,LockKind::Record)==RCOK,"young publishes via RDMA");
        auto waiter=std::async(std::launch::async,[&]{return a.acquire(old.id,LockMode::X,key,LockKind::Record);});
        check(waiter.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout,"older waits while victim still holds physical lock");
        if(policy==WaitPolicy::WoundWait) {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            bool wounded=false;
            while(std::chrono::steady_clock::now()<deadline)if(b.check(young.id).result==TxnResult::Wounded){wounded=true;break;}
            check(wounded,"wound delivered across independent control connections");
            check(b.prepare_commit(young.id).result==TxnResult::Wounded,"wounded session cannot commit");
        }
        check(pb.unlock_all(young.id)==RCOK,"victim physically releases before waking old");
        check(b.end(young.id).result==TxnResult::Ok,"victim releases metadata");
        check(waiter.get().result==TxnResult::Ok,"older transaction gets notified");
        check(pa.lock(old.id,LockMode::X,key,LockKind::Record)==RCOK,"old publishes via RDMA after wake");
        check(a.prepare_commit(old.id).result==TxnResult::Ok,"old commit fence");
        check(pa.unlock_all(old.id)==RCOK,"old physical cleanup");
        check(a.end(old.id).result==TxnResult::Ok,"old metadata cleanup");
    }
    // Exact predicates use the same physical RDMA range representation.
    auto reader=a.begin(WaitPolicy::NoWait,0,5000000000ULL),writer=b.begin(WaitPolicy::NoWait,0,5000000000ULL);
    const auto range=KeyRangeEnt::half_open(10,20);
    check(a.acquire(reader.id,LockMode::S,range,LockKind::Predicate).result==TxnResult::Ok,"predicate metadata");
    check(pa.lock(reader.id,LockMode::S,range,LockKind::Predicate)==RCOK,"predicate RDMA lock");
    check(b.acquire(writer.id,LockMode::X,KeyRangeEnt::point(15),LockKind::Predicate).result==TxnResult::Conflict,"empty predicate protects insertion");
    check(pa.unlock_all(reader.id)==RCOK,"predicate physical cleanup");
    check(a.end(reader.id).result==TxnResult::Ok && b.end(writer.id).result==TxnResult::Ok,"predicate metadata cleanup");
    auto exact_reader=a.begin(WaitPolicy::NoWait,0,5000000000ULL), exact_writer=b.begin(WaitPolicy::NoWait,0,5000000000ULL);
    const auto exact_range=KeyRangeEnt::half_open(10,20);
    check(exact_reader.result==TxnResult::Ok && exact_writer.result==TxnResult::Ok,"exact-range metadata");
    check(a.acquire(exact_reader.id,LockMode::S,exact_range,LockKind::ExactRange).result==TxnResult::Ok,"exact-range metadata grant");
    check(pa.lock(exact_reader.id,LockMode::S,exact_range,LockKind::ExactRange)==RCOK,"exact-range RDMA lock");
    check(b.acquire(exact_writer.id,LockMode::X,KeyRangeEnt::point(15),LockKind::ExactRange).result==TxnResult::Conflict,"exact-range metadata conflict");
    check(pb.lock(exact_writer.id,LockMode::X,KeyRangeEnt::point(15),LockKind::ExactRange)==AbortConflict,"exact-range RDMA conflict");
    check(pa.unlock_all(exact_reader.id)==RCOK && pb.unlock_all(exact_writer.id)==RCOK,"exact-range cleanup");
    check(a.end(exact_reader.id).result==TxnResult::Ok && b.end(exact_writer.id).result==TxnResult::Ok,"exact-range metadata cleanup");
    check(pa.get_lock_count()==0 && pb.get_lock_count()==0,"all physical locks removed");
    check(!pa.rdma_timeouts() && !pb.rdma_timeouts() && !pa.rdma_completion_errors() && !pb.rdma_completion_errors(),"no transport errors");
    std::cout<<"Independent-session RDMA wait-die/wound-wait, commit fence and predicate: OK\n";
}
