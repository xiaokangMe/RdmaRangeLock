#include "txn/workload.h"
#include <chrono>
#include <atomic>
#include "common/execution_context.h"
#include <iostream>
#include <stdexcept>
using namespace rrl;
void require(bool b,const char* text){if(!b)throw std::runtime_error(text);}
class ObservedIndex final:public BTreeIndex {
public:
    BTreeLatch tree;
    std::uint64_t inserts{0};
    bool expect_coroutines{true};
    std::atomic<unsigned> prepared{0};
    void prepare_thread(std::size_t,std::size_t) override {
        require(coroutine_active()==expect_coroutines,"incorrect execution context");
        ++prepared;
    }
    bool key_neighbors(idx_key_t k,IndexKeyNeighbors& out) const override{return tree.key_neighbors(k,out);}
    bool key_range_into(const KeyRangeEnt& range,std::vector<idx_key_t>& out) const override{return tree.key_range_into(range,out);}
    RC init(std::size_t n) override{return tree.init(n);}
    RC insert(idx_key_t k,void* p) override{++inserts;return tree.insert(k,p);}
    RC erase(idx_key_t k) override{return tree.erase(k);}
    bool lookup(idx_key_t k,void*& p) const override{return tree.lookup(k,p);}
    std::vector<KeyValue> scan_range(const KeyRangeEnt& r) const override{return tree.scan_range(r);}
    std::size_t size() const override{return tree.size();}
    RC apply_batch(const std::vector<IndexMutation>& v) override{return tree.apply_batch(v);}
};
int main() {
 try {
    for(const bool enabled:{false,true})
    for(const auto protocol:{PolicyProtocol::None,PolicyProtocol::Hierarchy}) {
        ObservedIndex index;require(index.init(8)==RCOK,"init");
        LockManager locks;locks.set_bucket_width(4);
        WorkloadConfig c;c.key_count=32;c.transaction_count=80;c.operations_per_transaction=4;c.threads=2;c.hierarchy_bucket_width=4;
        const auto trace=WorkloadTrace::generate(c);
        PolicyRunConfig cfg;cfg.protocol=protocol;cfg.use_coroutines=enabled;cfg.coroutines=enabled?2:1;cfg.coroutine_stack_bytes=65536;
        index.expect_coroutines=enabled;cfg.warmup_seconds=1;
        bool boundary=false;std::uint64_t seeded=0;
        cfg.measurement_begin=[&]{
            require(locks.get_lock_count()==0,"warmup did not drain physical locks");
            require(index.tree.validate(),"warmup damaged tree");
            seeded=index.inserts;require(seeded==c.key_count,"unexpected seed cardinality");
            boundary=true;
        };
        const auto s=run_policy_workload(index,locks,trace,cfg);
        require(boundary && !s.failed(),"warmup failed");
        require(index.prepared==c.threads*cfg.coroutines,"execution context count");
        require(enabled?s.policy.coroutine_switches>0:s.policy.coroutine_switches==0,"scheduler bypass");
        require(s.policy.warmup_committed>0 && s.policy.warmup_elapsed_ns>=1000000000ULL,"warmup absent");
        require(s.policy.logical_started==c.transaction_count,"warmup leaked into measured logical count");
        require(s.transactions==s.committed+s.aborted && s.transactions==s.policy.logical_started+s.policy.retries,"attempt accounting");
        require(s.policy.successful_attempt.count==s.committed && s.policy.logical_completion.count==s.committed,"histogram population");
        require(index.inserts==seeded,"measurement re-seeded warmup data");
        require(locks.get_lock_count()==0 && index.tree.validate(),"final invariants");
    }
    for(const bool enabled:{false,true}) {
    // Main-thread instrumentation failures must unblock/join all worker stacks.
    ObservedIndex index;index.init(8);LockManager locks;WorkloadConfig c;c.key_count=16;c.transaction_count=16;c.threads=2;
    PolicyRunConfig cfg;cfg.use_coroutines=enabled;cfg.coroutines=enabled?2:1;index.expect_coroutines=enabled;cfg.measurement_begin=[] {throw std::runtime_error("expected snapshot failure");};
    try {run_policy_workload(index,locks,WorkloadTrace::generate(c),cfg);return 2;}
    catch(const std::runtime_error& e){require(std::string(e.what())=="expected snapshot failure","wrong error");}
    }
    std::cout<<"Concurrent warmup, drain, retained dataset, isolated measurement and failure barrier: OK\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
