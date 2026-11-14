#include "../src/txn/compact_precision_plan.h"
#include "../src/txn/precision_plan.h"
#include "lock/record_lock_table.h"
#include "txn/record_workload.h"
#include <atomic>
#include <iostream>
#include <random>
#include <thread>

using namespace rrl;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)

static std::vector<RangeLock> reference(const TransactionTrace& txn,txnid_t id) {
    std::vector<RangeLock> locks;
    const auto add=[&](LockMode mode,const KeyRangeEnt& range,LockKind kind){locks.push_back({id,mode,kind,range});return true;};
    for(const auto& op:txn.operations) {
        if(op.kind==OperationKind::RangeRead) {
            if(op.key<op.end_key)add(LockMode::S,KeyRangeEnt::half_open(op.key,op.end_key),LockKind::PrecisionPredicate);
        } else CHECK(detail::precision_point(op,add));
    }
    return locks;
}

static TxnResult take(RecordLockTable& table,RecordLockTable::Client& client,
                      const std::vector<detail::CompactPrecisionPlans::OperationPlan>& plan) {
    for(const auto& op:plan)for(auto key=op.first;key<op.end;++key) {
        const auto result=client.acquire<WaitPolicy::NoWait>(*table.locate(key),op.mode).result;
        if(result!=TxnResult::Ok)return result;
    }
    return TxnResult::Ok;
}

static void differential() {
    std::mt19937_64 rng(20261114);
    WorkloadConfig cfg;cfg.kind=WorkloadKind::YcsbE;cfg.key_count=128;
    const std::vector<idx_key_t> edges{0,1,2,31,32,33,100,UINT64_MAX-1,UINT64_MAX};
    std::vector<TransactionTrace> txns;
    for(unsigned i=0;i<400;++i) {
        TransactionTrace txn;txn.txn_id=i+1;
        for(unsigned j=0,n=1+rng()%8;j<n;++j) {
            const auto kind=static_cast<OperationKind>(rng()%5);
            auto key=rng()%2?edges[rng()%edges.size()]:rng()%128;
            auto end=rng()%2?edges[rng()%edges.size()]:rng()%128;
            txn.operations.push_back({kind,key,end,100+i});
        }
        txns.push_back(std::move(txn));
    }
    const auto trace=WorkloadTrace::from_transactions(cfg,txns);
    detail::CompactPrecisionPlans plans(trace);
    RecordLockTable table(std::max<std::size_t>(1,plans.update_keys),{},2);
    RecordLockTable::Client a(table,0,plans.max_locks),b(table,1,plans.max_locks);
    for(unsigned i=0;i<20000;++i) {
        const auto x=rng()%txns.size(),y=rng()%txns.size();
        bool expected=false;
        for(const auto& lp:reference(txns[x],1))for(const auto& lu:reference(txns[y],2))expected|=lp.conflicts(lu);
        CHECK(a.begin_unordered(3000000000ULL).result==TxnResult::Ok);
        CHECK(b.begin_unordered(3000000000ULL).result==TxnResult::Ok);
        CHECK(take(table,a,plans.operations[x])==TxnResult::Ok);
        CHECK((take(table,b,plans.operations[y])==TxnResult::Conflict)==expected);
        a.finish();b.finish();CHECK(table.empty());
    }
}

static void boundaries_and_race() {
    WorkloadConfig cfg;cfg.kind=WorkloadKind::YcsbE;cfg.key_count=1;
    auto trace=WorkloadTrace::from_transactions(cfg,{
        {1,{{OperationKind::RangeRead,10,20,0}}},
        {2,{{OperationKind::Insert,15,0,1}}},
        {3,{{OperationKind::Insert,20,0,1}}},
        {4,{{OperationKind::Read,UINT64_MAX,0,0}}},
        {5,{{OperationKind::Update,UINT64_MAX,0,1}}},
        {6,{{OperationKind::RangeRead,UINT64_MAX-1,UINT64_MAX,0}}},
    });
    detail::CompactPrecisionPlans plans(trace);CHECK(plans.update_keys==3);
    RecordLockTable table(plans.update_keys,{},2);
    RecordLockTable::Client a(table,0,plans.max_locks),b(table,1,plans.max_locks);
    a.begin_unordered(3000000000ULL);b.begin_unordered(3000000000ULL);
    CHECK(take(table,a,plans.operations[0])==TxnResult::Ok);
    CHECK(take(table,b,plans.operations[1])==TxnResult::Conflict);b.finish();
    b.begin_unordered(3000000000ULL);CHECK(take(table,b,plans.operations[2])==TxnResult::Ok);
    a.finish();b.finish();
    a.begin_unordered(3000000000ULL);b.begin_unordered(3000000000ULL);
    CHECK(take(table,a,plans.operations[3])==TxnResult::Ok);
    CHECK(take(table,b,plans.operations[4])==TxnResult::Conflict);a.finish();b.finish();
    a.begin_unordered(3000000000ULL);b.begin_unordered(3000000000ULL);
    CHECK(take(table,a,plans.operations[5])==TxnResult::Ok);
    CHECK(take(table,b,plans.operations[4])==TxnResult::Ok);a.finish();b.finish();
    for(unsigned i=0;i<200;++i) {
        a.begin_unordered(3000000000ULL);b.begin_unordered(3000000000ULL);
        std::atomic<unsigned> ready{0},done{0};std::atomic<bool> go{false};
        TxnResult ar=TxnResult::Error,br=TxnResult::Error;
        auto compete=[&](auto& client,unsigned txn,TxnResult& result){
            ++ready;while(!go.load())std::this_thread::yield();
            result=take(table,client,plans.operations[txn]);++done;
            while(done.load()!=2)std::this_thread::yield();
        };
        std::thread one([&]{compete(a,0,ar);}),two([&]{compete(b,1,br);});
        while(ready.load()!=2)std::this_thread::yield();go=true;
        one.join();two.join();CHECK((ar==TxnResult::Ok)!=(br==TxnResult::Ok));
        a.finish();b.finish();CHECK(table.empty());
    }
}

static void mixed_equivalence() {
    WorkloadConfig cfg;cfg.kind=WorkloadKind::YcsbE;cfg.key_count=256;cfg.threads=1;
    const auto trace=WorkloadTrace::from_transactions(cfg,{
        {1,{{OperationKind::Insert,11,0,7},{OperationKind::RangeRead,0,32,0},
            {OperationKind::Read,11,0,0},{OperationKind::Delete,11,0,0},
            {OperationKind::RangeRead,0,32,0},{OperationKind::Insert,11,0,42},
            {OperationKind::Update,12,0,17},{OperationKind::Read,12,0,0}}},
        {2,{{OperationKind::RangeRead,0,32,0},{OperationKind::Delete,12,0,0},
            {OperationKind::Insert,UINT64_MAX,0,99},{OperationKind::Read,UINT64_MAX,0,0},
            {OperationKind::RangeRead,UINT64_MAX-1,UINT64_MAX,0}}},
    });
    PolicyRunConfig p;p.protocol=PolicyProtocol::Precision;p.use_coroutines=false;
    for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
        p.wait_policy=policy;
        BTreeLatch generic,compact;CHECK(generic.init(4)==RCOK);CHECK(compact.init(4)==RCOK);
        std::unordered_map<idx_key_t,std::uint64_t> gs,cs;
        CHECK(seed_index(generic,trace,gs));CHECK(seed_index(compact,trace,cs));p.index_preloaded=true;
        LockManager locks;const auto a=run_policy_workload(generic,locks,trace,p),b=run_compact_precision_workload(compact,trace,p);
        CHECK(!a.failed() && !b.failed());CHECK(a.committed==2 && b.committed==2);
        CHECK(a.checksum==b.checksum && a.rows_found==b.rows_found && a.operations==b.operations);
        std::vector<KeyValue> av,bv;generic.scan_range_into(KeyRangeEnt::unbounded(),av);compact.scan_range_into(KeyRangeEnt::unbounded(),bv);
        CHECK(av.size()==bv.size());
        for(std::size_t i=0;i<av.size();++i)CHECK(av[i].key==bv[i].key && *static_cast<std::uint64_t*>(av[i].value)==*static_cast<std::uint64_t*>(bv[i].value));
    }
    const auto readonly=WorkloadTrace::from_transactions(cfg,{{1,{{OperationKind::RangeRead,0,64,0},{OperationKind::Read,0,0,0}}}});
    detail::CompactPrecisionPlans plan(readonly);CHECK(plan.update_keys==0);
    BTreeLatch tree;CHECK(tree.init(4)==RCOK);p.index_preloaded=false;
    const auto result=run_compact_precision_workload(tree,readonly,p);CHECK(result.committed==1 && result.lock_acquires==0);
}

int main(){differential();boundaries_and_race();mixed_equivalence();std::cout<<"precision: 20000 reference Lp/Lu comparisons, 200 publication races, empty gaps, boundaries, implicit reads and own writes passed\n";}
