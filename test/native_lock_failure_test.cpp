#include "lock/native_policy.h"
#include "lock/remote_lock_manager.h"
#include "txn/shared_dataset.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace rrl;
namespace rrl {
// Actual failed WC on only the lock QP. Data and state QPs remain alive.
struct RemoteLockManagerTestPeer {
    static void fail_lock_qp(RemoteLockManager& locks, bool hold_gate) {
        auto& c = *locks.connections_.at(0)->connection;
        const auto remote = c.remote_memory();
        const auto gate = remote.address + offsetof(RemoteLockRegion, shards) + offsetof(RemoteLockShard, gate);
        std::uint64_t value = 0;
        if (hold_gate && (!c.compare_swap(gate, remote.rkey, 0, locks.connections_[0]->owner_token, value) || value))
            throw std::runtime_error("inject held gate");
        c.set_operation_tag(RdmaOpTag::LoadSlots);
        if (c.read(gate, remote.rkey ^ 0x80000000u, &value, sizeof(value)) || c.connected())
            throw std::runtime_error("expected failed lock WC");
    }
};
}
void require(bool ok, const char* message) { if(!ok)throw std::runtime_error(message); }
int main(int argc, char** argv) {
 try {
    const auto* host_env=std::getenv("RRL_TEST_RDMA_HOST");
    const auto* lock_env=std::getenv("RRL_TEST_RDMA_PORT");
    const auto* data_env=std::getenv("RRL_TEST_DATA_PORT");
    const auto* state_env=std::getenv("RRL_TEST_TXN_STATE_PORT");
    if(!lock_env || !data_env || !state_env)return 77;
    const std::string host=host_env?host_env:"127.0.0.1";
    const std::string mode=argc>1?argv[1]:"committing";
    require(mode=="clean" || mode=="running" || mode=="committing" || mode=="gate","unknown fault mode");
    const auto policy=argc>2?parse_wait_policy(argv[2]):WaitPolicy::WoundWait;
    RdmaConnection data;require(data.connect_client(host,std::stoi(data_env)),"data QP connect");
    const auto data_memory=data.remote_memory();
    OffsetBTreeHeader header{};
    require(data.read(data_memory.address,data_memory.rkey,&header,sizeof(header)) && !header.poison,"live data header");
    RemoteLockManager victim, contender;
    victim.use_coordinator_transaction_ids();contender.use_coordinator_transaction_ids();
    require(victim.connect(host,std::stoi(lock_env)) && contender.connect(host,std::stoi(lock_env)),"physical QPs");
    NativePolicyClient client(host,std::stoi(state_env),victim);
    const auto tx=client.begin(policy,0,10000000000ULL);
    require(tx.result==TxnResult::Ok,"native begin");
    const auto range=KeyRangeEnt::point(UINT64_MAX-2048);
    require(client.acquire(tx.id,LockMode::X,range,LockKind::Predicate).result==TxnResult::Ok,"victim lock");
    if(mode=="committing" || mode=="clean")
        require(client.prepare_commit(tx.id).result==TxnResult::Ok,"enter committing");
    if(mode=="clean") {
        require(victim.unlock_all(tx.id)==RCOK,"strict clean release");
        require(client.end(tx.id).result==TxnResult::Ok,"clean end");
    }
    RemoteLockManagerTestPeer::fail_lock_qp(victim,mode=="gate");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(;;) {
        const auto rc=contender.lock(tx.id+1,LockMode::X,range,LockKind::Predicate);
        if(mode=="clean") {
            require(rc==RCOK,"clean disconnect must preserve service");
            require(contender.unlock_all(tx.id+1)==RCOK,"clean contender release");break;
        }
        require(rc!=RCOK,"unsafe early unlock after lock QP loss");
        if(rc==ERROR)break;
        require(rc==AbortConflict || rc==AbortGate,"fault became ordinary capacity abort");
        require(std::chrono::steady_clock::now()<deadline,"native lock service failed to fence disconnect");
        std::this_thread::yield();
    }
    // This is specifically a lock-only failure, not process/data/state loss.
    if(mode!="clean") require(client.check(tx.id).result==TxnResult::Ok,"state QP must remain live");
    require(data.read(data_memory.address,data_memory.rkey,&header,sizeof(header)) && !header.poison,
            "data QP must remain live and unpoisoned");
    std::cout<<"Native physical QP disconnect "<<mode<<": no early grant; data/state alive\n";
    return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
