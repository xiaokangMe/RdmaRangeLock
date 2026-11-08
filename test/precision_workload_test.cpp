#include "txn/shared_dataset.h"
#include "index/griffin.h"
#include "lock/native_policy.h"
#include <atomic>
#include <thread>
#include <iostream>
#include <map>
#include <set>

using namespace rrl;
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);}while(0)
struct ObserveLocks final : IRangeLockManager {
    LockManager locks;
    std::set<idx_key_t> membership;
    RC lock(txnid_t t,LockMode m,const KeyRangeEnt& r,LockKind k,std::vector<txnid_t>* c=nullptr) override {
        if(k==LockKind::GriffinUpdate)membership.insert(r.StartKey);
        return locks.lock(t,m,r,k,c);
    }
    RC unlock(txnid_t t,const KeyRangeEnt& r,LockKind k) override{return locks.unlock(t,r,k);}
    RC unlock_all(txnid_t t) override{return locks.unlock_all(t);}
    std::size_t get_lock_count(txnid_t t=0) const override{return locks.get_lock_count(t);}
};
// Exact result validation under contention; every successful transaction
// increments both records. Stable values live beyond all index readers.
void concurrent_sum(WaitPolicy policy,PolicyProtocol protocol,bool hybrid) {
    constexpr unsigned threads=32,increments=8,pairs=threads/2;
    BTreeLatch latch;BTreeGriffin griffin;
    BTreeIndex& tree=hybrid?static_cast<BTreeIndex&>(griffin):static_cast<BTreeIndex&>(latch);
    CHECK(tree.init(8)==RCOK);
    std::uint64_t zero=0;
    for(unsigned k=0;k<2*pairs;++k)CHECK(tree.insert(k,&zero)==RCOK);
    std::vector<std::uint64_t> values(threads*increments);
    LockManager locks;LocalNativePolicyState state(threads);
    std::atomic<bool> error{false};std::atomic<unsigned> ready{0};
    std::vector<std::thread> workers;
    for(unsigned t=0;t<threads;++t)workers.emplace_back([&,t] {
        try {
            tree.prepare_thread(t,2);
            LocalNativePolicyClient client(state,locks);
            // Two writers per pair, all 32 workers active. NO_WAIT has no
            // finite retry/progress guarantee for 32 simultaneous S->X
            // upgrades on one pair; that is a liveness experiment, not a
            // lost-update correctness oracle.
            const idx_key_t key=2*(t%pairs);
            ++ready;while(ready.load()!=threads)std::this_thread::yield();
            std::vector<IndexMutation> writes;writes.reserve(2);
            for(unsigned n=0;n<increments && !error;++n) {
                bool committed=false;std::uint64_t age=0;
                for(unsigned retry=0;retry<100000 && !committed && !error;++retry) {
                    const auto begin=client.begin(policy,age,5000000000ULL);
                    CHECK(begin.result==TxnResult::Ok);age=begin.age;const auto id=begin.id;
                    const bool g=protocol==PolicyProtocol::Griffin;
                    auto result=client.acquire(id,LockMode::S,KeyRangeEnt::half_open(key,key+2),
                        g?LockKind::GriffinPredicate:LockKind::PrecisionPredicate).result;
                    for(unsigned k=0;k<2 && result==TxnResult::Ok;++k)
                        result=client.acquire(id,LockMode::X,KeyRangeEnt::point(key+k),
                            g?LockKind::Record:LockKind::PrecisionUpdate).result;
                    if(result==TxnResult::Ok) {
                        void *left=nullptr,*right=nullptr;
                        CHECK(tree.lookup(key,left) && tree.lookup(key+1,right));
                        CHECK(*static_cast<std::uint64_t*>(left)==*static_cast<std::uint64_t*>(right));
                        values[t*increments+n]=*static_cast<std::uint64_t*>(left)+1;
                        result=client.prepare_commit(id).result;
                    }
                    if(result==TxnResult::Ok) {
                        const auto* v=&values[t*increments+n];writes.clear();
                        writes.push_back({key,v,false});writes.push_back({key+1,v,false});
                        CHECK(tree.apply_batch(writes)==RCOK);committed=true;
                    }
                    CHECK(result!=TxnResult::Error);
                    if(client.has_physical_locks())CHECK(locks.unlock_all(id)==RCOK);
                    CHECK(client.end(id).result==TxnResult::Ok);
                    if(!committed)std::this_thread::yield();
                }
                CHECK(committed);
            }
        }catch(const std::exception& e){
            error=true;std::cerr<<"concurrent_sum policy="<<static_cast<unsigned>(policy)
                <<" protocol="<<static_cast<unsigned>(protocol)<<" hybrid="<<hybrid
                <<" worker="<<t<<" failure="<<e.what()<<"\n";
        }
    });
    for(auto& worker:workers)worker.join();
    CHECK(!error && locks.get_lock_count()==0);
    std::uint64_t total=0;
    for(unsigned pair=0;pair<pairs;++pair) {
        void *left=nullptr,*right=nullptr;
        CHECK(tree.lookup(2*pair,left) && tree.lookup(2*pair+1,right));
        CHECK(*static_cast<std::uint64_t*>(left)==2*increments);
        CHECK(*static_cast<std::uint64_t*>(right)==2*increments);
        total+=*static_cast<std::uint64_t*>(left);
    }
    CHECK(total==threads*increments);
    CHECK(hybrid?griffin.validate():latch.validate());
}
int main(){try {
    for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait})
    for(auto protocol:{PolicyProtocol::Precision,PolicyProtocol::Griffin}) {
        WorkloadConfig cfg;cfg.key_count=8;cfg.transaction_count=3;cfg.operations_per_transaction=1;cfg.threads=1;
        auto trace=WorkloadTrace::generate(cfg);
        // This is a mutable fixture; the production public trace stays immutable.
        auto& txns=const_cast<std::vector<TransactionTrace>&>(trace.transactions());
        txns[0].operations={
            {OperationKind::RangeRead,0,3,0},{OperationKind::Delete,1,0,0},
            {OperationKind::Read,1,0,0},{OperationKind::Insert,1,0,91},
            {OperationKind::RangeRead,0,3,0},{OperationKind::Update,0,0,81},
            {OperationKind::Read,0,0,0}};
        txns[1].operations={{OperationKind::Delete,4,0,0}};
        txns[2].operations={{OperationKind::Update,4,0,71},{OperationKind::Read,4,0,0}};
        std::map<idx_key_t,std::uint64_t> model;
        for(const auto& v:make_shared_seed(trace))model.insert(v);
        std::uint64_t checksum=0,found=0;
        for(const auto& t:txns)for(const auto& op:t.operations) {
            if(op.kind==OperationKind::Read) {const auto f=model.find(op.key);
                if(f!=model.end()){++found;checksum+=f->first^f->second;}}
            else if(op.kind==OperationKind::RangeRead) {
                for(auto f=model.lower_bound(op.key);f!=model.end() && f->first<op.end_key;++f) {++found;checksum+=f->first^f->second;}
            } else if(op.kind==OperationKind::Delete)model.erase(op.key);
            else model[op.key]=op.value;
        }
        for(bool hybrid:{false,true}) {
            if(hybrid && protocol!=PolicyProtocol::Griffin)continue;
            BTreeLatch btree;BTreeGriffin griffin;
            BTreeIndex& index=hybrid?static_cast<BTreeIndex&>(griffin):static_cast<BTreeIndex&>(btree);
            CHECK(index.init(8)==RCOK);ObserveLocks locks;
            PolicyRunConfig run;run.protocol=protocol;run.wait_policy=policy;run.use_coroutines=false;
            const auto stats=run_policy_workload(index,locks,trace,run);
            CHECK(!stats.failed() && stats.committed==3 && stats.aborted==0);
            CHECK(stats.checksum==checksum && stats.rows_found==found);
            CHECK(locks.get_lock_count()==0);
            if(hybrid)CHECK(griffin.validate());else CHECK(btree.validate());
            if(protocol==PolicyProtocol::Griffin)
                CHECK(locks.membership.count(4)); // missing Update must take Lu
        }
    }
    for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
        concurrent_sum(policy,PolicyProtocol::Precision,false);
        concurrent_sum(policy,PolicyProtocol::Griffin,false);
        concurrent_sum(policy,PolicyProtocol::Griffin,true);
    }
    std::cout<<"Precision/Griffin multi-operation delete, reinsert, missing upsert, own writes and policies: OK\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
