#include "txn/workload.h"
#include "txn/shared_dataset.h"
#include <unordered_map>
#include <iostream>
#include <set>
#include <stdexcept>
namespace rrl { bool seed_index(BTreeIndex&, const WorkloadTrace&, std::unordered_map<idx_key_t,std::uint64_t>&); }
using namespace rrl;
int main(){
  auto fail=[](bool ok,const char* msg){if(!ok)std::cerr<<"FAIL: "<<msg<<"\n";return ok;};
  WorkloadConfig a; a.kind=WorkloadKind::YcsbA;a.key_count=1000;a.transaction_count=100;a.operations_per_transaction=10;a.zipf_theta=0;a.seed=9;
  auto ta=WorkloadTrace::generate(a); if(!fail(ta.transactions().size()==100,"ycsb txn")||!fail(ta.transactions()[0].operations.size()==10,"ycsb ops"))return 1;
  WorkloadConfig e=a;e.kind=WorkloadKind::YcsbE;e.range_length=8;auto te=WorkloadTrace::generate(e);size_t ranges=0,inserts=0;std::vector<Operation> range_ops,insert_ops;for(auto& t:te.transactions())for(auto& o:t.operations){ranges+=o.kind==OperationKind::RangeRead;inserts+=o.kind==OperationKind::Insert;if(o.kind==OperationKind::RangeRead)range_ops.push_back(o);if(o.kind==OperationKind::Insert)insert_ops.push_back(o);}bool overlap=false;for(const auto& r:range_ops)for(const auto& i:insert_ops)if(i.key>=r.key&&i.key<r.end_key)overlap=true;if(!fail(ranges+inserts==1000,"ycsb e mix")||!fail(!insert_ops.empty(),"ycsb e inserts")||!fail(overlap,"ycsb e phantom overlap")||!fail(te.max_key()>=1000,"range upper bound"))return 1;
  WorkloadConfig p;p.kind=WorkloadKind::TpccPayment;p.key_count=10;p.transaction_count=4;auto tp=WorkloadTrace::generate(p);if(!fail(tp.transactions().size()==4,"payment txn")||!fail(tp.transactions()[0].operations.size()==3,"payment ops"))return 1;
  WorkloadConfig n=p;n.kind=WorkloadKind::TpccNewOrder;n.tpcc_items_per_order=5;auto tn=WorkloadTrace::generate(n);if(!fail(tn.transactions()[0].operations.size()==13,"new order ops"))return 1;
  for (auto kind : {WorkloadKind::YcsbA, WorkloadKind::YcsbB, WorkloadKind::YcsbE}) {
    for (size_t requests : {size_t(1),size_t(10),size_t(16)}) {
      WorkloadConfig c;c.kind=kind;c.key_count=16;c.transaction_count=20;
      c.operations_per_transaction=requests;c.range_length=8;c.zipf_theta=.99;
      auto first=WorkloadTrace::generate(c),second=WorkloadTrace::generate(c);
      for(size_t i=0;i<first.transactions().size();++i) {
        const auto& x=first.transactions()[i].operations;const auto& y=second.transactions()[i].operations;
        std::set<idx_key_t> keys;
        if(!fail(x.size()==requests,"configured request length"))return 1;
        for(size_t j=0;j<x.size();++j) {
          if(!fail(keys.insert(x[j].key).second,"Deneva transaction-local unique keys") ||
             !fail(x[j].key==y[j].key && x[j].kind==y[j].kind && x[j].end_key==y[j].end_key && x[j].value==y[j].value,"seed reproducibility"))return 1;
        }
      }
    }
  }
  WorkloadConfig invalid;invalid.key_count=1;invalid.operations_per_transaction=10;
  bool rejected=false;try{(void)WorkloadTrace::generate(invalid);}catch(const std::invalid_argument&){rejected=true;}
  if(!fail(rejected,"impossible unique request count rejected"))return 1;
  invalid.ycsb_unique_keys=false;
  if(!fail(WorkloadTrace::generate(invalid).transactions()[0].operations.size()==10,"explicit repeated-key regression mode"))return 1;
  // Compare actual legacy local-loader contents with the direct remote seed.
  for(auto kind:{WorkloadKind::YcsbA,WorkloadKind::YcsbB,WorkloadKind::YcsbE,WorkloadKind::TpccPayment,WorkloadKind::TpccNewOrder}) {
    for(bool unique:{false,true}) {
      WorkloadConfig c;c.kind=kind;c.key_count=128;c.transaction_count=200;c.operations_per_transaction=10;c.ycsb_unique_keys=unique;
      auto trace=WorkloadTrace::generate(c);BTreeSingle tree;tree.init(16);
      std::unordered_map<idx_key_t,std::uint64_t> values;
      if(!fail(seed_index(tree,trace,values),"legacy loader"))return 1;
      auto direct=make_shared_seed(trace);auto rows=tree.scan_range(KeyRangeEnt::unbounded());
      if(!fail(direct.size()==rows.size(),"direct seed cardinality"))return 1;
      for(size_t i=0;i<rows.size();++i)
        if(!fail(direct[i].first==rows[i].key && direct[i].second==*static_cast<uint64_t*>(rows[i].value),"exact direct seed content"))return 1;
    }
  }
  std::cout<<"workload trace: OK\n";
}
