// Correctness and bounded progress, never a formal performance comparison.
#include "txn/shared_dataset.h"
#include "index/griffin.h"
#include "lock/native_policy.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
using namespace rrl;
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);}while(0)
struct WitnessLocks final : LockManager {
    std::promise<void> conflict;
    std::atomic<bool> seen{false};
    RC lock(txnid_t id,LockMode mode,const KeyRangeEnt& range,LockKind kind,
            std::vector<txnid_t>* conflicts=nullptr) override {
        const auto rc=LockManager::lock(id,mode,range,kind,conflicts);
        if(rc==AbortConflict && !seen.exchange(true))conflict.set_value();
        return rc;
    }
};
void upgrade(WaitPolicy policy,bool griffin) {
    WitnessLocks locks;LocalNativePolicyState state(2);
    LocalNativePolicyClient old(state,locks),young(state,locks);
    const auto a=old.begin(policy,0,5000000000ULL),b=young.begin(policy,0,5000000000ULL);
    CHECK(a.result==TxnResult::Ok && b.result==TxnResult::Ok && a.age<b.age);
    const auto range=KeyRangeEnt::point(7);
    const auto p=griffin?LockKind::GriffinPredicate:LockKind::PrecisionPredicate;
    const auto u=griffin?LockKind::GriffinUpdate:LockKind::PrecisionUpdate;
    CHECK(old.acquire(a.id,LockMode::S,range,p).result==TxnResult::Ok);
    CHECK(young.acquire(b.id,LockMode::S,range,p).result==TxnResult::Ok);
    auto waiter=std::async(std::launch::async,[&]{return old.acquire(a.id,LockMode::X,range,u);});
    CHECK(locks.conflict.get_future().wait_for(std::chrono::seconds(2))==std::future_status::ready);
    if(policy==WaitPolicy::NoWait) {
        CHECK(waiter.get().result==TxnResult::Conflict);
        CHECK(young.acquire(b.id,LockMode::X,range,u).result==TxnResult::Conflict);
    }else {
        if(policy==WaitPolicy::WaitDie)
            CHECK(young.acquire(b.id,LockMode::X,range,u).result==TxnResult::Conflict);
        else {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(young.check(b.id).result==TxnResult::Ok && std::chrono::steady_clock::now()<deadline)
                std::this_thread::yield();
            CHECK(young.check(b.id).result==TxnResult::Wounded);
        }
        // Wounding does not steal locks, and a waiter cannot see partial data.
        CHECK(locks.get_lock_count()==2);
        CHECK(waiter.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready);
    }
    CHECK(locks.unlock_all(b.id)==RCOK);CHECK(young.end(b.id).result==TxnResult::Ok);
    if(policy!=WaitPolicy::NoWait) {
        CHECK(waiter.get().result==TxnResult::Ok);
        CHECK(old.prepare_commit(a.id).result==TxnResult::Ok);
    }
    CHECK(locks.unlock_all(a.id)==RCOK);CHECK(old.end(a.id).result==TxnResult::Ok);
    const auto retry=young.begin(policy,b.age,5000000000ULL);
    CHECK(retry.result==TxnResult::Ok && retry.age==b.age && retry.id!=b.id);
    CHECK(young.end(retry.id).result==TxnResult::Ok && locks.get_lock_count()==0);
}
// The uniform driver's automatically generated seed is scoped to the run.
// Keep this oracle's values alive through the final check, and tell the
// driver the fixture has already been loaded.
struct PreloadedFixture final : BTreeIndex {
    BTreeIndex& tree;
    explicit PreloadedFixture(BTreeIndex& t):tree(t){}
    RC init(std::size_t n) override{return tree.init(n);}
    RC insert(idx_key_t k,void* v) override{return tree.insert(k,v);}
    RC erase(idx_key_t k) override{return tree.erase(k);}
    RC apply_batch(const std::vector<IndexMutation>& v) override{return tree.apply_batch(v);}
    bool lookup(idx_key_t k,void*& v) const override{return tree.lookup(k,v);}
    std::vector<KeyValue> scan_range(const KeyRangeEnt& r) const override{return tree.scan_range(r);}
    void scan_range_into(const KeyRangeEnt& r,std::vector<KeyValue>& out) const override{tree.scan_range_into(r,out);}
    std::size_t size() const override{return tree.size();}
    bool preloaded() const override{return true;}
    bool healthy() const override{return tree.healthy();}
    void prepare_thread(std::size_t n,std::size_t c) override{tree.prepare_thread(n,c);}
    void prepare_write_capacity(std::size_t n) override{tree.prepare_write_capacity(n);}
    void finish_thread() noexcept override{tree.finish_thread();}
};
int main(int argc,char** argv) {try {
    const unsigned seconds=argc>1?static_cast<unsigned>(std::stoul(argv[1])):1;
    if(!seconds || seconds>10)throw std::invalid_argument("probe seconds must be 1..10");
    for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait})
        for(bool griffin:{false,true})upgrade(policy,griffin);
    for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait})
    for(unsigned variant=0;variant<3;++variant) {
        WorkloadConfig cfg;cfg.key_count=2;cfg.transaction_count=512;
        cfg.operations_per_transaction=3;cfg.range_length=2;cfg.ycsb_unique_keys=false;cfg.threads=32;cfg.duration_seconds=seconds;
        auto trace=WorkloadTrace::generate(cfg);
        auto& txns=const_cast<std::vector<TransactionTrace>&>(trace.transactions());
        for(std::size_t n=0;n<txns.size();++n)
            txns[n].operations={{OperationKind::RangeRead,0,2,0},
                {OperationKind::Update,0,0,1000+n},{OperationKind::Update,1,0,1000+n}};
        BTreeLatch latch;BTreeGriffin griffin;
        BTreeIndex& index=variant==2?static_cast<BTreeIndex&>(griffin):static_cast<BTreeIndex&>(latch);
        CHECK(index.init(8)==RCOK);
        std::uint64_t seed[2]={0,1};
        CHECK(index.insert(0,&seed[0])==RCOK && index.insert(1,&seed[1])==RCOK);
        PreloadedFixture fixture(index);LockManager locks;
        PolicyRunConfig run;run.protocol=variant==0?PolicyProtocol::Precision:PolicyProtocol::Griffin;
        run.wait_policy=policy;run.use_coroutines=false;run.coroutines=1;
        run.max_attempts=64;run.wait_timeout_ms=100;
        const auto s=run_policy_workload(fixture,locks,trace,run);const auto& m=s.policy;
        CHECK(!s.failed() && locks.get_lock_count()==0);
        CHECK(m.logical_started>0 && m.logical_started==s.committed+m.unfinished+m.logical_failed);
        CHECK(s.transactions==s.committed+s.aborted);
        CHECK(m.retries==m.completed_retries+m.unfinished_retries);
        CHECK(m.logical_completion.count==s.committed && m.active_workers>0);
        CHECK(m.workers_without_commit<=m.active_workers);
        if(policy==WaitPolicy::NoWait)CHECK(m.lock_wait.total_ns==0);
        // A wall-clock stress interval cannot prove scheduler fairness or
        // starvation freedom. The deterministic upgrade schedule above
        // requires older-waiter progress; this open schedule reports stalls
        // as first-class outcomes instead of misclassifying them as success.
        if(!s.committed) {
            CHECK(m.workers_without_commit==m.active_workers);
            CHECK(m.unfinished==m.logical_started);
            CHECK(m.retry_exhausted || m.deadline_aborts);
        }
        CHECK(variant==2?griffin.validate():latch.validate());
        void *a=nullptr,*b=nullptr;CHECK(index.lookup(0,a) && index.lookup(1,b));
        const auto left=*static_cast<std::uint64_t*>(a),right=*static_cast<std::uint64_t*>(b);
        if(s.committed)CHECK(left==right && left>=1000 && left<1000+cfg.transaction_count);
        else CHECK(left==0 && right==1);
        std::cout<<"{\"policy\":\""<<wait_policy_name(policy)<<"\",\"variant\":"<<variant
            <<",\"threads\":32,\"coroutines\":0,\"seconds\":"<<seconds
            <<",\"logical_started\":"<<m.logical_started<<",\"committed\":"<<s.committed
            <<",\"unfinished\":"<<m.unfinished<<",\"retries\":"<<m.retries
            <<",\"retry_exhausted\":"<<m.retry_exhausted<<",\"deadline_aborts\":"<<m.deadline_aborts
            <<",\"wound_aborts\":"<<m.wound_aborts<<",\"lock_wait_ns\":"<<m.lock_wait.total_ns
            <<",\"logical_latency_samples\":"<<m.logical_completion.count
            <<",\"logical_p99_ns\":"<<m.logical_completion.p99_ns
            <<",\"active_workers\":"<<m.active_workers<<",\"workers_without_commit\":"<<m.workers_without_commit
            <<",\"worker_min_committed\":"<<m.worker_min_committed<<",\"worker_max_committed\":"<<m.worker_max_committed
            <<",\"progress_observed\":"<<(s.committed?"true":"false")
            <<",\"completion_fraction\":"<<static_cast<double>(s.committed)/m.logical_started<<"}\n"<<std::flush;
    }
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
