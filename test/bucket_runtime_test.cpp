#include "txn/workload.h"
#include <iostream>
using namespace rrl;
void require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
struct ConversionFailure:LockManager {
    RC fault{ERROR};
    RC convert_lock(txnid_t,LockMode,const KeyRangeEnt&,LockKind,std::vector<txnid_t>*) override{return fault;}
};
int main(){try {
    for(auto workload:{WorkloadKind::YcsbA,WorkloadKind::YcsbE})
    for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
        WorkloadConfig c;c.kind=workload;c.key_count=32;c.transaction_count=40;c.operations_per_transaction=8;
        c.hierarchy_bucket_width=32;c.range_length=4;c.threads=1;
        const auto trace=WorkloadTrace::generate(c);
        BTreeLatch baseline;require(baseline.init(8)==RCOK,"baseline init");LockManager original;
        PolicyRunConfig run;run.protocol=PolicyProtocol::Hierarchy;run.wait_policy=policy;
        const auto expected=run_policy_workload(baseline,original,trace,run);
        for(bool deescalate:{false,true}) {
            BTreeLatch tree;require(tree.init(8)==RCOK,"runtime init");LockManager locks;
            run.hierarchy_escalation_threshold=2;run.hierarchy_deescalate_after=deescalate?4:0;
            const auto result=run_policy_workload(tree,locks,trace,run);
            require(!result.failed() && result.committed==40 && result.aborted==0,"conversion transaction success");
            require(result.checksum==expected.checksum && result.rows_found==expected.rows_found,"same logical results through runtime conversions");
            require(result.policy.escalation_attempts==result.policy.escalations+result.policy.escalation_failures,"escalation counts");
            require(result.policy.deescalation_attempts==result.policy.deescalations+result.policy.deescalation_failures,"deescalation counts");
            if(workload==WorkloadKind::YcsbA)require(result.policy.escalations>0,"actual runtime escalation");
            if(deescalate)require(result.policy.deescalations>0,"actual runtime deescalation");
            require(tree.validate() && locks.get_lock_count()==0,"conversion invariants");
        }
    }
    for(auto fault:{ERROR,WAIT,AbortConflict,AbortGate,AbortCapacity}) {
        WorkloadConfig c;c.key_count=32;c.transaction_count=10;c.operations_per_transaction=8;c.hierarchy_bucket_width=32;
        BTreeLatch tree;require(tree.init(8)==RCOK,"fault init");ConversionFailure locks;locks.fault=fault;
        PolicyRunConfig run;run.protocol=PolicyProtocol::Hierarchy;run.hierarchy_escalation_threshold=2;
        const auto result=run_policy_workload(tree,locks,WorkloadTrace::generate(c),run);
        if(fault==ERROR || fault==WAIT)require(result.failed() && result.lock_errors==1 && result.transactions==1,"hard conversion failure stops run");
        else require(!result.failed() && result.committed==10 && result.policy.escalation_failures>0,"failed opportunistic conversion keeps fine locks");
        require(locks.get_lock_count()==0 && tree.validate(),"failed conversion cleanup");
    }
    std::cout<<"Runtime bucket escalation/deescalation, logical result equivalence and failure preservation: OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
