#include "txn/shared_dataset.h"
#include <map>
#include <iostream>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
using namespace rrl;
// Deterministically reject every nonempty commit before mutation. The
// workload must retain capacity failures when aggregating worker statistics.
struct CapacityIndex final : BTreeIndex {
    BTreeLatch tree;
    RC init(std::size_t order) override { return tree.init(order); }
    RC insert(idx_key_t k,void* v) override { return tree.insert(k,v); }
    RC erase(idx_key_t k) override { return tree.erase(k); }
    bool lookup(idx_key_t k,void*& v) const override { return tree.lookup(k,v); }
    std::vector<KeyValue> scan_range(const KeyRangeEnt& r) const override { return tree.scan_range(r); }
    std::size_t size() const override { return tree.size(); }
    RC apply_batch(const std::vector<IndexMutation>& changes) override {
        return changes.empty()?RCOK:AbortCapacity;
    }
};
int main(){try {
    for(auto kind:{WorkloadKind::YcsbA,WorkloadKind::YcsbE}) {
        WorkloadConfig cfg;cfg.ycsb_unique_keys=false;cfg.kind=kind;cfg.key_count=4;cfg.transaction_count=100;
        cfg.operations_per_transaction=20;cfg.range_length=4;cfg.threads=1;cfg.seed=71;
        auto trace=WorkloadTrace::generate(cfg);std::map<idx_key_t,std::uint64_t> model;
        for(const auto& p:make_shared_seed(trace))model.insert(p);
        std::uint64_t checksum=0,found=0;
        for(const auto& txn:trace.transactions())for(const auto& op:txn.operations) {
            if(op.kind==OperationKind::Read) {auto it=model.find(op.key);if(it!=model.end()){++found;checksum+=it->first^it->second;}}
            else if(op.kind==OperationKind::RangeRead)for(auto it=model.lower_bound(op.key);it!=model.end() && it->first<op.end_key;++it){++found;checksum+=it->first^it->second;}
            else if(op.kind==OperationKind::Delete)model.erase(op.key);
            else model[op.key]=op.value;
        }
        for(auto protocol:{PolicyProtocol::Record,PolicyProtocol::Gap,PolicyProtocol::NextKey,PolicyProtocol::Hierarchy,PolicyProtocol::Predicate})
            for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
                BTreeLatch index;CHECK(index.init(16)==RCOK);LockManager locks;
                PolicyRunConfig run;run.protocol=protocol;run.wait_policy=policy;
                auto result=run_policy_workload(index,locks,trace,run);
                CHECK(!result.failed() && result.committed==100 && result.aborted==0);
                CHECK(result.checksum==checksum && result.rows_found==found);
                CHECK(index.validate() && locks.get_lock_count()==0);
            }
    }
    // One key and many operations ensure every trace transaction has a write.
    for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
        WorkloadConfig cfg;cfg.ycsb_unique_keys=false;cfg.kind=WorkloadKind::YcsbA;cfg.key_count=1;
        cfg.transaction_count=20;cfg.operations_per_transaction=32;cfg.threads=2;cfg.seed=71;
        const auto trace=WorkloadTrace::generate(cfg);
        for(const auto& txn:trace.transactions())
            CHECK(std::any_of(txn.operations.begin(),txn.operations.end(),[](const Operation& op){
                return op.kind==OperationKind::Update || op.kind==OperationKind::Insert;
            }));
        CapacityIndex index;CHECK(index.init(16)==RCOK);LockManager locks;
        PolicyRunConfig run;run.protocol=PolicyProtocol::Hierarchy;run.wait_policy=policy;run.max_attempts=3;
        const auto result=run_policy_workload(index,locks,trace,run);
        CHECK(!result.failed() && result.committed==0 && result.lock_capacity_aborts>0);
        CHECK(result.aborted==result.transactions && result.policy.unfinished==20);
        CHECK(result.lock_capacity_aborts<=result.aborted && result.policy.retry_exhausted==20);
        CHECK(index.tree.validate() && locks.get_lock_count()==0);
    }
    std::cout<<"Own-write visibility and capacity abort aggregation across policies: OK\n";return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
