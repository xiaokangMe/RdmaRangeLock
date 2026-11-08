#include "lock/native_policy.h"
#include "lock/remote_lock_manager.h"
#include "txn/shared_dataset.h"
#include "precision_contract.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace rrl;
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);}while(0)
int main(int argc,char** argv){try {
    const char *hp=getenv("RRL_TEST_RDMA_HOST"),*lp=getenv("RRL_TEST_RDMA_PORT"),
               *dp=getenv("RRL_TEST_DATA_PORT"),*sp=getenv("RRL_TEST_TXN_STATE_PORT"),
               *cfg=getenv("RRL_TEST_CONFIG");
    if(!lp || !dp || !sp || !cfg)return 77;
    const std::string host=hp?hp:"127.0.0.1";
    if(argc>1 && std::string(argv[1])=="contract") {
        RemoteLockManager locks;locks.set_connection_count(2);
        CHECK(locks.connect(host,std::stoi(lp)));precision_contract::check(locks);
        CHECK(!locks.rdma_timeouts() && !locks.rdma_completion_errors());
        std::cout<<"Precision/Griffin remote slot contract: OK\n";return 0;
    }
    CHECK(argc>=3);
    const std::string protocol=argv[1],mode=argv[2];
    CHECK(protocol=="precision-lock" || protocol=="griffin-pl");
    WorkloadConfig workload;RdmaTreeConfig tc;std::uint16_t ignored=0;
    load_shared_dataset_config(cfg,workload,tc,ignored);
    const auto seed=make_shared_seed(WorkloadTrace::generate(workload));
    BTreeRdma index;CHECK(index.connect(host,std::stoi(dp),1,tree_seed_fingerprint(seed)));
    CHECK(index.init(tc.order)==RCOK);index.prepare_thread(0,4);index.prepare_write_capacity(2);
    const idx_key_t key=UINT64_MAX-1024;const auto range=KeyRangeEnt::half_open(key,key+2);
    auto get=[&](idx_key_t k){void* v=nullptr;return index.lookup(k,v)?*static_cast<std::uint64_t*>(v):0;};
    if(mode=="read") {
        CHECK(argc==4);const auto expected=std::stoull(argv[3]);
        CHECK(get(key)==expected && get(key+1)==expected);CHECK(index.validate());index.finish_thread();
        std::cout<<"Cross-process exact two-key sum: "<<expected<<" OK\n";return 0;
    }
    CHECK(mode=="increment" && argc==5);
    const auto policy=parse_wait_policy(argv[3]);const auto count=std::stoull(argv[4]);
    const bool griffin=protocol=="griffin-pl";
    RemoteLockManager locks;locks.use_coordinator_transaction_ids();locks.set_connection_count(1);
    CHECK(locks.connect(host,std::stoi(lp)));NativePolicyClient client(host,std::stoi(sp),locks);
    std::vector<IndexMutation> writes;writes.reserve(2);
    for(std::uint64_t n=0;n<count;++n) {
        bool committed=false;std::uint64_t age=0;
        for(unsigned retry=0;retry<10000 && !committed;++retry) {
            const auto begun=client.begin(policy,age,5000000000ULL);
            CHECK(begun.result==TxnResult::Ok);age=begun.age;const auto id=begun.id;
            auto outcome=client.acquire(id,LockMode::S,range,
                griffin?LockKind::GriffinPredicate:LockKind::PrecisionPredicate).result;
            for(unsigned k=0;k<2 && outcome==TxnResult::Ok;++k) {
                const auto point=KeyRangeEnt::point(key+k);
                // Precision implicit read is covered by the range Lp above.
                // Griffin uses separate record isolation and membership Lu.
                if(griffin)outcome=client.acquire(id,LockMode::X,point,LockKind::Record).result;
                if(outcome==TxnResult::Ok)outcome=client.acquire(id,LockMode::X,point,
                    griffin?LockKind::GriffinUpdate:LockKind::PrecisionUpdate).result;
            }
            std::uint64_t value=0;
            if(outcome==TxnResult::Ok) {
                value=get(key);CHECK(get(key+1)==value);++value;
                outcome=client.prepare_commit(id).result;
            }
            if(outcome==TxnResult::Ok) {
                writes.clear();writes.push_back({key,&value,false});writes.push_back({key+1,&value,false});
                CHECK(index.apply_batch(writes)==RCOK);committed=true;
            }
            CHECK(outcome!=TxnResult::Error);
            if(client.has_physical_locks())CHECK(locks.unlock_all(id)==RCOK);
            CHECK(client.end(id).result==TxnResult::Ok);
        }
        CHECK(committed);
    }
    CHECK(index.validate());index.finish_thread();
    CHECK(!locks.rdma_timeouts() && !locks.rdma_completion_errors());
    std::cout<<protocol<<" native RDMA two-key atomic increments: "<<count<<" OK\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
