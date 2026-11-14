#include "lock/classic_lock_table.h"
#include "txn/record_workload.h"
#include "../src/txn/key_range_plan.h"
#include <future>
#include <iostream>
#include <random>

using namespace rrl;
static constexpr std::uint64_t budget=3000000000ULL;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
void differential() {
    const std::vector<idx_key_t> keys={0,1,10,11,20,30,UINT64_MAX};
    std::vector<KeyRangeEnt> ranges{KeyRangeEnt::unbounded()};
    for(auto a:keys) {
        ranges.push_back(KeyRangeEnt::point(a));
        ranges.push_back(KeyRangeEnt::from_left(a));
        ranges.push_back(KeyRangeEnt::from_left(a,Bound::Open));
        ranges.push_back(KeyRangeEnt::to_right(a));
        ranges.push_back(KeyRangeEnt::to_right(a,Bound::Closed));
        for(auto b:keys)if(a<b) {
            ranges.push_back(KeyRangeEnt::closed(a,b));
            ranges.push_back(KeyRangeEnt::half_open(a,b));
            ranges.push_back(KeyRangeEnt::open(a,b));
            auto r=KeyRangeEnt::open(a,b);r.end_bound=Bound::Closed;ranges.push_back(r);
        }
    }
    std::mt19937_64 rng(987);
    for(auto protocol:{PolicyProtocol::NextKey,PolicyProtocol::PriorKey,PolicyProtocol::Gap}) {
        ClassicLockTable table(keys,2,protocol);
        ClassicLockTable::Client a(table.authority(),0,64),b(table.authority(),1,64);
        for(unsigned iteration=0;iteration<10000;++iteration) {
            RangeLock first{1,rng()%2?LockMode::S:LockMode::X,
                protocol==PolicyProtocol::PriorKey?LockKind::PriorKey:LockKind::NextKey,ranges[rng()%ranges.size()]};
            auto second=first;second.txn_id=2;second.range=ranges[rng()%ranges.size()];second.mode=rng()%2?LockMode::S:LockMode::X;
            if(protocol==PolicyProtocol::Gap)for(auto* req:{&first,&second}) {
                const auto kind=rng()%3;
                if(kind==0){req->kind=LockKind::Gap;req->mode=LockMode::S;}
                else {req->kind=kind==1?LockKind::Record:LockKind::InsertIntent;
                    req->range=KeyRangeEnt::point(keys[rng()%keys.size()]);
                    if(req->kind==LockKind::InsertIntent)req->mode=LockMode::X;}
            }
            CHECK(a.begin(0,budget).result==TxnResult::Ok);CHECK(b.begin(0,budget).result==TxnResult::Ok);
            CHECK(table.acquire<WaitPolicy::NoWait>(a,first.mode,first.range,first.kind).result==TxnResult::Ok);
            const auto actual=table.acquire<WaitPolicy::NoWait>(b,second.mode,second.range,second.kind).result;
            CHECK((actual==TxnResult::Conflict)==first.conflicts(second));
            a.finish();b.finish();CHECK(table.authority().empty());
        }
    }
}
template<WaitPolicy Policy>void phantom_and_wait(PolicyProtocol protocol) {
    ClassicLockTable table({0,10,15,20,30},2,protocol);
    ClassicLockTable::Client old(table.authority(),0,32),young(table.authority(),1,32);
    auto scan=protocol==PolicyProtocol::Gap?LockKind::Gap:protocol==PolicyProtocol::NextKey?LockKind::NextKey:LockKind::PriorKey;
    auto insert=protocol==PolicyProtocol::Gap?LockKind::InsertIntent:scan;
    auto span=KeyRangeEnt::half_open(10,20);auto point=KeyRangeEnt::point(15);
    old.begin(0,budget);young.begin(0,budget);
    CHECK(table.acquire<Policy>(old,LockMode::S,span,scan).result==TxnResult::Ok);
    if constexpr(Policy==WaitPolicy::WaitDie) {
        CHECK(table.acquire<Policy>(young,LockMode::X,point,insert).result==TxnResult::Conflict);
        old.finish();young.finish();
    } else {
        auto waiter=std::async(std::launch::async,[&]{return table.acquire<Policy>(young,LockMode::X,point,insert);});
        CHECK(old.prepare_commit<Policy>()==TxnResult::Ok);old.finish();
        CHECK(waiter.get().result==TxnResult::Ok);young.finish();
    }
    old.begin(0,budget);young.begin(0,budget);
    CHECK(table.acquire<Policy>(young,LockMode::S,span,scan).result==TxnResult::Ok);
    auto waiter=std::async(std::launch::async,[&]{return table.acquire<Policy>(old,LockMode::X,point,insert);});
    if constexpr(Policy==WaitPolicy::WoundWait) {
        const auto limit=RecordLockTable::Clock::now()+std::chrono::seconds(2);
        while(young.check()!=TxnResult::Wounded)CHECK(RecordLockTable::Clock::now()<limit);
    }
    young.finish();CHECK(waiter.get().result==TxnResult::Ok);old.finish();CHECK(table.authority().empty());
}
void executor_equivalence() {
    WorkloadConfig config;config.key_count=32;config.transaction_count=1;config.threads=1;config.range_length=32;
    const std::vector<Operation> ops={{OperationKind::RangeRead,0,32,0},
        {OperationKind::Insert,15,0,150},{OperationKind::RangeRead,10,20,0},
        {OperationKind::Delete,10,0,0},{OperationKind::Read,10,0,0},
        {OperationKind::Insert,10,0,100},{OperationKind::Read,15,0,0},
        {OperationKind::Insert,UINT64_MAX,0,9},{OperationKind::Read,UINT64_MAX,0,0}};
    const auto trace=WorkloadTrace::from_transactions(config,{{1,ops}});
    for(auto protocol:{PolicyProtocol::NextKey,PolicyProtocol::Gap,PolicyProtocol::PriorKey})
        for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
            BTreeLatch a,b;a.init(4);b.init(4);LockManager generic;
            PolicyRunConfig cfg;cfg.protocol=protocol;cfg.wait_policy=policy;cfg.use_coroutines=false;
            const auto expected=run_policy_workload(a,generic,trace,cfg);
            const auto actual=run_compact_key_workload(b,trace,cfg);
            CHECK(!expected.failed() && !actual.failed());CHECK(expected.committed==1 && actual.committed==1);
            CHECK(expected.checksum==actual.checksum);CHECK(expected.rows_found==actual.rows_found);
            CHECK(a.validate() && b.validate() && a.size()==b.size());
        }
}
// Prove next/prior compact scans use boundary observations plus the data
// scan, and do not silently fall back to interior key enumeration. Include
// missing keys, sparse inserts, deletion and split-capable order-4 pages.
void boundary_only_execution() {
    class BoundaryIndex final : public BTreeIndex {
    public:
        BTreeLatch tree;unsigned enumerations=0;
        RC init(std::size_t order)override{return tree.init(order);}
        RC insert(idx_key_t k,void* v)override{return tree.insert(k,v);}
        RC erase(idx_key_t k)override{return tree.erase(k);}
        bool lookup(idx_key_t k,void*& v)const override{return tree.lookup(k,v);}
        std::size_t size()const override{return tree.size();}
        std::vector<KeyValue> scan_range(const KeyRangeEnt& r)const override{return tree.scan_range(r);}
        void scan_range_into(const KeyRangeEnt& r,std::vector<KeyValue>& v)const override{tree.scan_range_into(r,v);}
        bool key_neighbors(idx_key_t k,IndexKeyNeighbors& n)const override{return tree.key_neighbors(k,n);}
        bool key_range_into(const KeyRangeEnt&,std::vector<idx_key_t>&)const override{
            ++const_cast<BoundaryIndex*>(this)->enumerations;return false;
        }
    };
    WorkloadConfig config;config.key_count=8;config.transaction_count=3;config.threads=1;config.range_length=32;
    auto trace=WorkloadTrace::from_transactions(config,{
        {1,{{OperationKind::Insert,12,0,120},{OperationKind::RangeRead,6,15,0}}},
        {2,{{OperationKind::Delete,7,0,0},{OperationKind::RangeRead,6,15,0}}},
        {3,{{OperationKind::Insert,9,0,90},{OperationKind::RangeRead,8,15,0},{OperationKind::RangeRead,40,45,0}}}});
    for(auto protocol:{PolicyProtocol::NextKey,PolicyProtocol::PriorKey})
        for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
            BTreeLatch reference;reference.init(4);BoundaryIndex actual;actual.init(4);LockManager locks;
            PolicyRunConfig cfg;cfg.protocol=protocol;cfg.wait_policy=policy;cfg.use_coroutines=false;
            const auto a=run_policy_workload(reference,locks,trace,cfg);
            const auto b=run_compact_key_workload(actual,trace,cfg);
            CHECK(!a.failed() && !b.failed());CHECK(a.committed==3 && b.committed==3);
            CHECK(a.checksum==b.checksum && a.rows_found==b.rows_found);
            CHECK(actual.enumerations==0 && actual.tree.validate());
        }
}
void validation_and_membership() {
    for(auto protocol:{PolicyProtocol::NextKey,PolicyProtocol::Gap,PolicyProtocol::PriorKey}) {
        ClassicLockTable table({10,15,20,UINT64_MAX},2,protocol);
        ClassicLockTable::Client scan(table.authority(),0,32),insert(table.authority(),1,32);
        const auto kind=protocol==PolicyProtocol::Gap?LockKind::Gap:
            protocol==PolicyProtocol::NextKey?LockKind::NextKey:LockKind::PriorKey;
        CHECK(scan.begin_unordered(budget).result==TxnResult::Ok);
        CHECK(table.acquire<WaitPolicy::NoWait>(scan,LockMode::S,KeyRangeEnt::open(20,10),kind).result==TxnResult::Error);
        CHECK(table.authority().empty());scan.finish();
        BTreeLatch index;CHECK(index.init(4)==RCOK);std::uint64_t value=1;
        CHECK(index.insert(10,&value)==RCOK);CHECK(index.insert(20,&value)==RCOK);
        const Operation read{OperationKind::RangeRead,10,20,0},write{OperationKind::Insert,15,0,1};
        detail::KeyObservation observation;std::vector<detail::KeyRequest> plan;
        auto take_plan=[&](ClassicLockTable::Client& client,const Operation& op) {
            CHECK(detail::observe_keys(index,op,observation));detail::key_plan(op,observation,protocol,plan);
            for(const auto& request:plan) {
                const auto result=table.acquire<WaitPolicy::NoWait>(client,request.mode,request.range,request.kind).result;
                if(result!=TxnResult::Ok)return result;
            }
            return TxnResult::Ok;
        };
        scan.begin_unordered(budget);insert.begin_unordered(budget);
        CHECK(take_plan(scan,read)==TxnResult::Ok);
        // A concurrent insert into the currently empty gap cannot write back.
        auto blocked=std::async(std::launch::async,[&]{return take_plan(insert,write);});
        CHECK(blocked.get()==TxnResult::Conflict);insert.finish();
        std::vector<idx_key_t> rows;
        CHECK(index.key_range_into(KeyRangeEnt::half_open(10,20),rows) && rows==std::vector<idx_key_t>{10});
        scan.finish();insert.begin_unordered(budget);
        CHECK(take_plan(insert,write)==TxnResult::Ok);CHECK(index.insert(15,&value)==RCOK);insert.finish();
        CHECK(index.key_range_into(KeyRangeEnt::half_open(10,20),rows) && rows==std::vector<idx_key_t>({10,15}));
        CHECK(index.validate() && table.authority().empty());
    }
}
int main() {try {
    differential();
    validation_and_membership();
    for(auto protocol:{PolicyProtocol::NextKey,PolicyProtocol::Gap,PolicyProtocol::PriorKey}) {
        phantom_and_wait<WaitPolicy::WaitDie>(protocol);phantom_and_wait<WaitPolicy::WoundWait>(protocol);
    }
    executor_equivalence();
    boundary_only_execution();
    std::cout<<"Classic CAS: 30000 pairwise reference cases, boundaries, phantoms, wait/wound, splits and own writes OK\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
