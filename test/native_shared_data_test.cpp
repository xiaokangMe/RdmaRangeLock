#include "lock/native_policy.h"
#include "lock/remote_lock_manager.h"
#include "txn/shared_dataset.h"
#include "../src/lock/native_txn_layout.h"
#include <chrono>
#include <thread>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace rrl;
void require(bool b,const char* what){if(!b)throw std::runtime_error(what);}
int main(int argc,char** argv) {
 try {
    const auto* hp=std::getenv("RRL_TEST_RDMA_HOST");
    const auto* lp=std::getenv("RRL_TEST_RDMA_PORT");
    const auto* dp=std::getenv("RRL_TEST_DATA_PORT");
    const auto* sp=std::getenv("RRL_TEST_TXN_STATE_PORT");
    const auto* cp=std::getenv("RRL_TEST_CONFIG");
    if(!lp || !dp || !sp)return 77;
    const std::string host=hp?hp:"127.0.0.1";
    if(argc>1 && std::string(argv[1])=="--check-state-poison") {
        LockManager unused;
        NativePolicyClient c(host,std::stoi(sp),unused);
        require(c.begin(WaitPolicy::WoundWait,0,1000000000ULL).result==TxnResult::Error,
                "uncertain data did not poison native transactions");
        std::cout<<"Native state rejects new transactions after data failure\n";return 0;
    }
    if(argc>1 && std::string(argv[1])=="--poison-active") {
        RdmaConnection probe;require(probe.connect_client(host,std::stoi(dp)),"poison probe");
        const auto memory=probe.remote_memory();
        std::uint64_t latch=0;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        do {
            require(probe.read(memory.address,memory.rkey,&latch,8),"probe active structure latch");
            require(std::chrono::steady_clock::now()<deadline,"no active workload observed");
            if(!latch)std::this_thread::yield();
        }while(!latch);
        const std::uint64_t poison=1;
        require(probe.write(memory.address+offsetof(OffsetBTreeHeader,poison),memory.rkey,&poison,8),"inject data poison");
        std::cout<<"Injected data failure during an active transaction\n";return 0;
    }
    WorkloadConfig workload;RdmaTreeConfig tree;std::uint16_t ignored=0;
    load_shared_dataset_config(cp?cp:"experiments/configs/rdma_shared_smoke.json",workload,tree,ignored);
    const auto seed=make_shared_seed(WorkloadTrace::generate(workload));
    BTreeRdma index;require(index.connect(host,std::stoi(dp),1,tree_seed_fingerprint(seed)),"data connect");
    require(index.init(tree.order)==RCOK,"remote init");index.prepare_thread(0,4);
    const auto key=UINT64_MAX-1024;
    const auto range=KeyRangeEnt::half_open(key,key+2);
    auto get=[&](idx_key_t k) {
        void* p=nullptr;
        return index.lookup(k,p)?*static_cast<std::uint64_t*>(p):0;
    };
    if(argc>1 && std::string(argv[1])=="--read") {
        const auto expected=std::stoull(argv[2]);
        require(get(key)==expected && get(key+1)==expected,"cross-process exact shared sum");
    } else {
        const auto count=argc>1?std::stoull(argv[1]):50;
        const auto policy=argc>2?parse_wait_policy(argv[2]):WaitPolicy::WoundWait;
        RemoteLockManager locks;locks.use_coordinator_transaction_ids();locks.set_connection_count(1);
        require(locks.connect(host,std::stoi(lp)),"lock connect");
        NativePolicyClient client(host,std::stoi(sp),locks);
        std::vector<IndexMutation> writes;writes.reserve(2);
        for(std::uint64_t i=0;i<count;++i) {
            bool committed=false;std::uint64_t age=0;
            for(int retry=0;retry<10000 && !committed;++retry) {
                auto b=client.begin(policy,age,5000000000ULL);require(b.result==TxnResult::Ok,"begin");age=b.age;
                auto outcome=client.acquire(b.id,LockMode::X,range,LockKind::NextKey).result;
                std::uint64_t value=0;
                if(outcome==TxnResult::Ok) {
                    value=get(key);require(get(key+1)==value,"torn range update");++value;
                    outcome=client.prepare_commit(b.id).result;
                }
                if(outcome==TxnResult::Ok) {
                    writes.clear();writes.push_back({key,&value,false});writes.push_back({key+1,&value,false});
                    require(index.apply_batch(writes)==RCOK,"shared writeback");committed=true;
                }
                require(outcome!=TxnResult::Error,"native lock error");
                if(client.has_physical_locks())require(locks.unlock_all(b.id)==RCOK,"physical release");
                require(client.end(b.id).result==TxnResult::Ok,"end after release");
            }
            require(committed,"retry budget exhausted");
        }
    }
    require(index.validate(),"remote tree invariants");index.finish_thread();
    std::cout<<"Native shared two-record range atomicity / exact sum OK\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
