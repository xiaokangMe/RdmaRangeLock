#include "../src/txn/compact_exact_plan.h"
#include "lock/record_lock_table.h"
#include "txn/record_workload.h"
#include <iostream>
#include <random>

using namespace rrl;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)

static bool conflict(const TransactionTrace& a,const TransactionTrace& b) {
    for(const auto& x:a.operations)for(const auto& y:b.operations) {
        if((x.kind==OperationKind::RangeRead && x.key>=x.end_key) ||
           (y.kind==OperationKind::RangeRead && y.key>=y.end_key))continue;
        auto make=[](const Operation& op,txnid_t id){return RangeLock{id,
            op.kind==OperationKind::Read || op.kind==OperationKind::RangeRead?LockMode::S:LockMode::X,
            LockKind::ExactRange,op.kind==OperationKind::RangeRead?KeyRangeEnt::half_open(op.key,op.end_key):KeyRangeEnt::point(op.key)};};
        if(make(x,1).conflicts(make(y,2)))return true;
    }
    return false;
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
    detail::CompactExactPlans plans(trace);
    RecordLockTable table(plans.nodes,{},2);
    RecordLockTable::Client a(table,0,plans.max_locks),b(table,1,plans.max_locks);
    for(unsigned i=0;i<20000;++i) {
        const auto x=rng()%txns.size(),y=rng()%txns.size();
        CHECK(a.begin_unordered(3000000000ULL).result==TxnResult::Ok);
        CHECK(b.begin_unordered(3000000000ULL).result==TxnResult::Ok);
        for(const auto& r:plans.plans[x])CHECK(a.acquire<WaitPolicy::NoWait>(*table.locate(r.node),r.mode).result==TxnResult::Ok);
        bool blocked=false;
        for(const auto& r:plans.plans[y])if(b.acquire<WaitPolicy::NoWait>(*table.locate(r.node),r.mode).result==TxnResult::Conflict){blocked=true;break;}
        CHECK(blocked==conflict(txns[x],txns[y]));
        a.finish();b.finish();CHECK(table.empty());
    }
}

static void phantom_and_boundaries() {
    WorkloadConfig cfg;cfg.kind=WorkloadKind::YcsbE;cfg.key_count=1;
    const auto trace=WorkloadTrace::from_transactions(cfg,{
        {1,{{OperationKind::RangeRead,10,20,0}}},
        {2,{{OperationKind::Insert,15,0,1}}},
        {3,{{OperationKind::Insert,20,0,1}}},
        {4,{{OperationKind::RangeRead,20,30,0}}},
    });
    detail::CompactExactPlans plans(trace);RecordLockTable table(plans.nodes,{},2);
    RecordLockTable::Client scan(table,0,plans.max_locks),writer(table,1,plans.max_locks);
    auto take=[&](auto& client,unsigned txn){
        for(const auto& r:plans.plans[txn]) {
            const auto result=client.template acquire<WaitPolicy::NoWait>(*table.locate(r.node),r.mode).result;
            if(result!=TxnResult::Ok)return result;
        }
        return TxnResult::Ok;
    };
    scan.begin_unordered(3000000000ULL);writer.begin_unordered(3000000000ULL);
    CHECK(take(scan,0)==TxnResult::Ok);CHECK(take(writer,1)==TxnResult::Conflict);
    writer.finish();writer.begin_unordered(3000000000ULL);
    CHECK(take(writer,2)==TxnResult::Ok);writer.finish();
    writer.begin_unordered(3000000000ULL);CHECK(take(writer,3)==TxnResult::Ok);writer.finish();
    scan.finish();writer.begin_unordered(3000000000ULL);CHECK(take(writer,1)==TxnResult::Ok);writer.finish();
    CHECK(table.empty());
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
    PolicyRunConfig p;p.protocol=PolicyProtocol::ExactRange;p.use_coroutines=false;
    for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
        p.wait_policy=policy;
        BTreeLatch generic,compact;CHECK(generic.init(4)==RCOK);CHECK(compact.init(4)==RCOK);
        // Keep seed value storage alive while inspecting the final indexes.
        std::unordered_map<idx_key_t,std::uint64_t> generic_seed,compact_seed;
        CHECK(seed_index(generic,trace,generic_seed));CHECK(seed_index(compact,trace,compact_seed));
        p.index_preloaded=true;
        LockManager locks;
        const auto a=run_policy_workload(generic,locks,trace,p),b=run_compact_exact_workload(compact,trace,p);
        CHECK(!a.failed() && !b.failed());CHECK(a.committed==2 && b.committed==2);
        CHECK(a.checksum==b.checksum && a.rows_found==b.rows_found && a.operations==b.operations);
        std::vector<KeyValue> av,bv;generic.scan_range_into(KeyRangeEnt::unbounded(),av);compact.scan_range_into(KeyRangeEnt::unbounded(),bv);
        CHECK(av.size()==bv.size());
        for(std::size_t i=0;i<av.size();++i)CHECK(av[i].key==bv[i].key && *static_cast<std::uint64_t*>(av[i].value)==*static_cast<std::uint64_t*>(bv[i].value));
    }
}

int main(){differential();phantom_and_boundaries();mixed_equivalence();std::cout<<"exact plans: 20000 conflict comparisons, empty gaps, half-open boundaries, own writes and split equivalence passed\n";}
