#include "txn/workload.h"
#include "src/txn/compact_hierarchy_plan.h"
#include "src/txn/compact_exact_plan.h"
#include <iostream>
#include <iomanip>
#include <array>
using namespace rrl;
int main(){
std::cout<<"theta,prefix64_pct,prefix4096_pct,write_txn_pct,avg_bucket_locks,avg_record_locks,avg_S_buckets,avg_SIX_buckets,avg_plan_locks,bucket0_txn_pct,bucket0_SIX_txn_pct,exact_avg_locks\n";
for(double z:{0.,.1,.3,.5,.7,.9,1.1,1.3,1.5}){
 WorkloadConfig c;c.kind=WorkloadKind::YcsbE;c.key_count=2097152;c.transaction_count=5000;c.operations_per_transaction=10;c.range_length=32;c.threads=32;c.zipf_theta=z;c.seed=8;c.hierarchy_bucket_width=64;
 auto t=WorkloadTrace::generate(c);double p64=0,p4096=0,w=0,b=0,r=0,s=0,six=0,b0=0,b0six=0;
 for(auto& txn:t.transactions()){
 bool writes=false;
 for(auto& op:txn.operations){p64+=op.key<64;p4096+=op.key<4096;writes|=op.kind==OperationKind::Insert;}
 w+=writes;
 auto plan=detail::hierarchy_plan(txn,64);
 for(auto& q:plan){
 if(q.phase==1){++b;s+=q.mode==LockMode::S;six+=q.mode==LockMode::SIX;
 if(q.range.StartKey==0){++b0;b0six+=q.mode==LockMode::SIX;}}
 if(q.phase==2)++r;
 }}
 detail::CompactExactPlans exact(t);double ex=0;for(auto& p:exact.plans)ex+=p.size();
 auto n=double(c.transaction_count);
 std::cout<<std::setprecision(9)<<z<<","<<p64/500<<","<<p4096/500<<","<<w/n*100<<","<<b/n<<","<<r/n<<","<<s/n<<","<<six/n<<","<<(b+r)/n<<","<<b0/n*100<<","<<b0six/n*100<<","<<ex/n<<"\n";
}}
