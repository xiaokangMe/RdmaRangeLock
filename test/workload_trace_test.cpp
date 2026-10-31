#include "txn/workload.h"
#include <iostream>
using namespace rrl;
int main(){
  auto fail=[](bool ok,const char* msg){if(!ok)std::cerr<<"FAIL: "<<msg<<"\n";return ok;};
  WorkloadConfig a; a.kind=WorkloadKind::YcsbA;a.key_count=1000;a.transaction_count=100;a.operations_per_transaction=10;a.zipf_theta=0;a.seed=9;
  auto ta=WorkloadTrace::generate(a); if(!fail(ta.transactions().size()==100,"ycsb txn")||!fail(ta.transactions()[0].operations.size()==10,"ycsb ops"))return 1;
  WorkloadConfig e=a;e.kind=WorkloadKind::YcsbE;e.range_length=8;auto te=WorkloadTrace::generate(e);size_t ranges=0,inserts=0;std::vector<Operation> range_ops,insert_ops;for(auto& t:te.transactions())for(auto& o:t.operations){ranges+=o.kind==OperationKind::RangeRead;inserts+=o.kind==OperationKind::Insert;if(o.kind==OperationKind::RangeRead)range_ops.push_back(o);if(o.kind==OperationKind::Insert)insert_ops.push_back(o);}bool overlap=false;for(const auto& r:range_ops)for(const auto& i:insert_ops)if(i.key>=r.key&&i.key<r.end_key)overlap=true;if(!fail(ranges+inserts==1000,"ycsb e mix")||!fail(!insert_ops.empty(),"ycsb e inserts")||!fail(overlap,"ycsb e phantom overlap")||!fail(te.max_key()>=1000,"range upper bound"))return 1;
  WorkloadConfig p;p.kind=WorkloadKind::TpccPayment;p.key_count=10;p.transaction_count=4;auto tp=WorkloadTrace::generate(p);if(!fail(tp.transactions().size()==4,"payment txn")||!fail(tp.transactions()[0].operations.size()==3,"payment ops"))return 1;
  WorkloadConfig n=p;n.kind=WorkloadKind::TpccNewOrder;n.tpcc_items_per_order=5;auto tn=WorkloadTrace::generate(n);if(!fail(tn.transactions()[0].operations.size()==13,"new order ops"))return 1;
  std::cout<<"workload trace: OK\n";
}