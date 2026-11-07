#include "txn/shared_dataset.h"
#include "lock/native_policy.h"
#include <iostream>
#include <functional>
using namespace rrl;
void require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
struct SnapshotIndex : BTreeIndex {
    BTreeLatch tree;
    mutable std::size_t scans{0},lookups{0};
    mutable std::vector<std::uint64_t> values;
    RC init(std::size_t n) override{return tree.init(n);}
    RC insert(idx_key_t k,void* v) override{return tree.insert(k,v);}
    RC erase(idx_key_t k) override{return tree.erase(k);}
    std::size_t size() const override{return tree.size();}
    bool lookup(idx_key_t k,void*& v) const override{++lookups;return tree.lookup(k,v);}
    std::vector<KeyValue> scan_range(const KeyRangeEnt& r) const override {
        ++scans;auto rows=tree.scan_range(r);values.resize(rows.size());
        for(std::size_t n=0;n<rows.size();++n) {
            values[n]=*static_cast<std::uint64_t*>(rows[n].value);rows[n].value=&values[n];
        }
        return rows;
    }
};
struct WindowLocks : LockManager {
    std::function<void()> window;
    bool fired{false};
    LockKind trigger{LockKind::Record};
    RC lock(txnid_t t,LockMode m,const KeyRangeEnt& r,LockKind k,std::vector<txnid_t>* c) override {
        if(!fired && k==trigger && m==LockMode::S) {fired=true;window();}
        return LockManager::lock(t,m,r,k,c);
    }
};
struct PrefixFailure : LockManager {
    unsigned calls{0};
    RC lock(txnid_t t,LockMode m,const KeyRangeEnt& r,LockKind k,std::vector<txnid_t>* c) override {
        if(++calls==2)return AbortCapacity;
        return LockManager::lock(t,m,r,k,c);
    }
};
int main(){try {
    WorkloadConfig c;c.kind=WorkloadKind::YcsbE;c.key_count=16;c.transaction_count=1;
    c.operations_per_transaction=1;c.range_length=8;c.threads=1;
    auto trace=WorkloadTrace::generate(c);
    while(trace.transactions()[0].operations[0].kind!=OperationKind::RangeRead) {
        ++c.seed;trace=WorkloadTrace::generate(c);
    }
    const auto op=trace.transactions()[0].operations[0];const auto target=op.key;
    for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait})
    for(auto protocol:{PolicyProtocol::Record,PolicyProtocol::Gap})
    for(bool remove:{false,true}) {
        SnapshotIndex index;require(index.init(8)==RCOK,"init");WindowLocks locks;
        locks.trigger=protocol==PolicyProtocol::Gap?LockKind::Gap:LockKind::Record;
        std::uint64_t replacement=0x12345678,expected=0,found=0;
        for(const auto& row:make_shared_seed(trace))if(row.first>=op.key && row.first<op.end_key) {
            ++found;expected+=row.first^(row.first==target?replacement:row.second);
        }
        locks.window=[&]{
            require(locks.LockManager::lock(99999,LockMode::X,KeyRangeEnt::point(target),LockKind::Record)==RCOK,"writer before reader record grant");
            require((remove?index.erase(target):index.insert(target,&replacement))==RCOK,"window mutation");
            require(locks.unlock_all(99999)==RCOK,"writer commit");
        };
        PolicyRunConfig cfg;cfg.protocol=protocol;cfg.wait_policy=policy;cfg.max_attempts=1;
        const auto s=run_policy_workload(index,locks,trace,cfg);
        require(locks.fired && !s.failed() && locks.get_lock_count()==0,"window completed without leaks");
        require(index.scans==2 && index.lookups==0,"two scans replace per-record descents");
        if(remove && protocol==PolicyProtocol::Record)
            require(s.committed==0 && s.aborted==1,"missing candidate aborts");
        else {
            if(remove){expected-=target^replacement;--found;}
            require(s.committed==1 && s.checksum==expected && s.rows_found==found,"post-lock snapshot values");
        }
    }
    PrefixFailure physical;LocalNativePolicyState state(1);LocalNativePolicyClient client(state,physical);
    const auto tx=client.begin(WaitPolicy::NoWait,0,1000000000ULL);
    require(tx.result==TxnResult::Ok,"begin");
    require(client.acquire_batch(tx.id,LockMode::S,{KeyRangeEnt::point(1),KeyRangeEnt::point(2)},LockKind::Record).result==TxnResult::Conflict,"prefix failure");
    require(client.has_physical_locks() && physical.get_lock_count(tx.id)==1,"successful prefix tracked");
    require(physical.unlock_all(tx.id)==RCOK && client.end(tx.id).result==TxnResult::Ok && physical.get_lock_count()==0,"prefix cleanup");
    std::cout<<"Scan post-lock values, delete window and failed batch-prefix cleanup: OK\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
