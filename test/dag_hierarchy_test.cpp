#include "dag_contract.h"
#include "txn/shared_dataset.h"
#include "../src/txn/leaf_hierarchy.h"
#include <cstdlib>
#include <iostream>
using namespace rrl;
int main(int argc,char** argv){try {
    dag_contract::topology();
    if(argc==3 && std::string(argv[1])=="--remote") {
        WorkloadConfig c;RdmaTreeConfig t;std::uint16_t ignored;load_shared_dataset_config(argv[2],c,t,ignored);
        const auto records=detail::object_record_keys(WorkloadTrace::generate(c));
        const std::string host=std::getenv("RRL_TEST_RDMA_HOST");const auto port=std::stoi(std::getenv("RRL_TEST_RDMA_PORT"));
        ObjectLockManager wrong,wrong_paths,legacy,locks;
        dag_contract::require(wrong.configure_dag(32,2) && !wrong.connect(host,port,records,1),"reject mismatched bucket topology");
        dag_contract::require(wrong_paths.configure_dag(16,3) && !wrong_paths.connect(host,port,records,1),"reject mismatched parent paths");
        dag_contract::require(!legacy.connect(host,port,records,1),"reject legacy physical-node client against DAG");
        dag_contract::require(locks.configure_dag(16,2) && locks.connect(host,port,records,1),"remote DAG exact geometry");
        dag_contract::check(locks);return 0;
    }
    ObjectLockManager locks;dag_contract::require(locks.configure_dag(16,2) && locks.init_local({10,11,13},32,1,128),"local DAG");dag_contract::check(locks);
    {
        BTreeLatch tree;tree.init(8);WorkloadConfig c;c.key_count=16;c.transaction_count=1;
        PolicyRunConfig cfg;cfg.protocol=PolicyProtocol::Hierarchy;cfg.dag_hierarchy=true;cfg.hierarchy_index_paths=3;
        dag_contract::rejected([&]{run_policy_workload(tree,locks,WorkloadTrace::generate(c),cfg);});
    }
    for(auto workload:{WorkloadKind::YcsbA,WorkloadKind::YcsbE})
    for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
        WorkloadConfig c;c.kind=workload;c.key_count=32;c.transaction_count=24;c.operations_per_transaction=8;c.range_length=4;c.hierarchy_bucket_width=16;
        const auto trace=WorkloadTrace::generate(c);BTreeLatch original;original.init(8);LockManager base;
        PolicyRunConfig run;run.protocol=PolicyProtocol::Hierarchy;run.wait_policy=policy;
        const auto reference=run_policy_workload(original,base,trace,run);
        for(std::size_t paths:{2,3})for(std::size_t read=0;read<paths;++read) {
            BTreeLatch tree;tree.init(8);ObjectLockManager object;
            dag_contract::require(object.configure_dag(16,paths) && object.init_local(detail::object_record_keys(trace),detail::dag_node_capacity(trace,16,paths),1,512),"runtime DAG init");
            run.dag_hierarchy=true;run.hierarchy_index_paths=paths;run.hierarchy_read_path=read;
            const auto result=run_policy_workload(tree,object,trace,run);
            dag_contract::require(!result.failed() && result.committed==24 && result.aborted==0 &&
                result.checksum==reference.checksum && result.rows_found==reference.rows_found,"DAG logical workload results");
            dag_contract::require(tree.validate() && object.get_lock_count()==0,"DAG runtime invariants");
        }
    }
    std::cout<<"DAG topology, all-parent writes, canonical records and workload paths: OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
