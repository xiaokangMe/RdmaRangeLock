#include "txn/record_workload.h"
#include "txn/shared_dataset.h"
#include "lock/record_lock_table.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

using namespace rrl;
static void check(bool ok,const char* text) { if(!ok)throw std::runtime_error(text); }

int main() {
    // Equal-key entries span multiple leaf/internal splits; a closed-start
    // scan must include the leftmost copy, and neighbors must remain strict.
    for(auto order:{3u,4u,16u}) {
        BTreeLatch tree;check(tree.init(order)==RCOK,"init");
        std::uint64_t value=7;
        for(auto key:{0u,10000u,20000u,30000u})check(tree.insert(key,&value)==RCOK,"seed");
        for(int i=0;i<128;++i)check(tree.insert_duplicate(10000,&value)==RCOK,"duplicate");
        check(tree.size()==132 && tree.validate(),"duplicate splits validate");
        auto rows=tree.scan_range(KeyRangeEnt::half_open(10000,20000));
        check(rows.size()==129,"closed start includes every duplicate");
        auto open=KeyRangeEnt::closed(10000,20000);open.start_bound=Bound::Open;
        check(tree.scan_range(open).size()==1,"open start skips every duplicate");
        IndexKeyNeighbors n;check(tree.key_neighbors(10000,n),"neighbors");
        check(n.present && n.has_predecessor && n.predecessor==0 && n.has_successor && n.successor==20000,"strict duplicate neighbors");
        check(tree.key_neighbors(12500,n) && !n.present && n.predecessor==10000 && n.successor==20000,"missing-key neighbors");
        std::vector<idx_key_t> keys;check(tree.key_range_into(KeyRangeEnt::unbounded(),keys) && keys.size()==4,"distinct lock coordinates");
    }
    {
        BTreeLatch tree;tree.init(8);std::uint64_t value=3;tree.insert(10000,&value);
        std::atomic<bool> ok{true};std::vector<std::thread> workers;
        for(int w=0;w<4;++w)workers.emplace_back([&]{for(int n=0;n<400;++n)if(tree.insert_duplicate(10000,&value)!=RCOK)ok=false;});
        for(auto& worker:workers)worker.join();
        check(ok && tree.size()==1601 && tree.validate(),"concurrent duplicate splits");
        check(tree.scan_range(KeyRangeEnt::point(10000)).size()==1601,"concurrent duplicates visible");
    }
    WorkloadConfig c;c.kind=WorkloadKind::YcsbE;c.deneva_ycsb_insert=true;
    c.key_count=128;c.transaction_count=1000;c.range_length=8;c.zipf_theta=0;
    const auto generated=WorkloadTrace::generate(c),repeat=WorkloadTrace::generate(c);
    std::size_t inserts=0,scans=0;
    for(std::size_t i=0;i<generated.transactions().size();++i) {
        const auto& x=generated.transactions()[i].operations;
        const auto& y=repeat.transactions()[i].operations;
        check(x.size()==10,"preserve ten operations");
        for(std::size_t j=0;j<x.size();++j) {
            check(x[j].key==y[j].key && x[j].end_key==y[j].end_key && x[j].kind==y[j].kind,"fixed request reproducibility");
            check(x[j].key<128*kDenevaKeyScale,"encoded key domain");
            if(x[j].kind==OperationKind::Insert)++inserts;
            else {++scans;check(x[j].key%10000==0 && x[j].end_key%10000==0,"scaled scan bounds");}
        }
    }
    check(inserts>400 && inserts<600 && inserts+scans==10000,"95/5 operation mix");
    check(make_shared_seed(generated).size()==128,"full seed, no removed insert holes");
    RecordLockTable locks(128,{12500,10000},1,10000);
    check(locks.locate(10000)!=nullptr && locks.locate(12500)!=nullptr && locks.locate(12501)==nullptr,"strided dense/sparse lock lookup");
    check(locks.lock_bytes()==129*8,"no 10000-fold lock allocation");

    // Replaying a committed transaction adds another physical entry.
    // Own inserted copies must be visible in all subsequent scans.
    c.key_count=3;c.transaction_count=1;c.range_length=1;
    TransactionTrace txn;txn.txn_id=1;
    txn.operations.push_back({OperationKind::Insert,12500,0,17});
    for(int i=0;i<9;++i)txn.operations.push_back({OperationKind::RangeRead,10000,20000,0});
    const auto trace=WorkloadTrace::from_transactions(c,{txn});
    for(auto protocol:{PolicyProtocol::Record,PolicyProtocol::NextKey,PolicyProtocol::Gap,
                       PolicyProtocol::PriorKey,PolicyProtocol::Hierarchy,PolicyProtocol::ExactRange,PolicyProtocol::Precision}) {
        for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
            for(bool compact:{false,true}) {
                BTreeLatch tree;tree.init(3);PolicyRunConfig cfg;cfg.protocol=protocol;cfg.wait_policy=policy;cfg.use_coroutines=false;
                std::unordered_map<idx_key_t,std::uint64_t> seed;
                check(seed_index(tree,trace,seed),"persistent seed for replay");cfg.index_preloaded=true;
                auto run=[&] {
                    if(!compact){LockManager lm;lm.set_bucket_width(workload_bucket_width(c));return run_policy_workload(tree,lm,trace,cfg);}
                    if(protocol==PolicyProtocol::Record)return run_compact_record_workload(tree,trace,cfg);
                    if(protocol==PolicyProtocol::Precision)return run_compact_precision_workload(tree,trace,cfg);
                    if(protocol==PolicyProtocol::Hierarchy)return run_compact_hierarchy_workload(tree,trace,cfg);
                    if(protocol==PolicyProtocol::ExactRange)return run_compact_exact_workload(tree,trace,cfg);
                    return run_compact_key_workload(tree,trace,cfg);
                };
                auto first=run();check(!first.failed() && first.committed==1 && tree.size()==4 && first.rows_found==18,"first replay and own insert reads");
                cfg.index_preloaded=true;auto second=run();
                check(!second.failed() && second.committed==1 && tree.size()==5 && second.rows_found==27,"committed replay appends equal-key entry");
                check(tree.validate(),"replay tree valid");
            }
        }
    }
    {
        c.key_count=3;
        TransactionTrace repeated;repeated.txn_id=1;
        repeated.operations={{OperationKind::Insert,12500,0,17},{OperationKind::Insert,12500,0,19},
                             {OperationKind::RangeRead,10000,20000,0}};
        auto repeated_trace=WorkloadTrace::from_transactions(c,{repeated});
        for(auto protocol:{PolicyProtocol::Record,PolicyProtocol::NextKey,PolicyProtocol::Gap,
                           PolicyProtocol::PriorKey,PolicyProtocol::Hierarchy,PolicyProtocol::ExactRange,PolicyProtocol::Precision}) {
            BTreeLatch tree;tree.init(3);PolicyRunConfig cfg;cfg.protocol=protocol;cfg.use_coroutines=false;
            LockManager lm;lm.set_bucket_width(workload_bucket_width(c));
            auto stats=run_policy_workload(tree,lm,repeated_trace,cfg);
            check(!stats.failed() && stats.committed==1 && tree.size()==5 && stats.rows_found==3,"two own appended equal-key rows");
            check(stats.checksum==((10000^1)+(12500^17)+(12500^19)),"each own duplicate retains its value");
        }
    }
    std::cout<<"Deneva Insert: fixed decimal keys, duplicate splits, concurrent append, seven protocols / three policies / compact+generic OK\n";
}
