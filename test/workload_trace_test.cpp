#include "txn/workload.h"
#include "txn/shared_dataset.h"
#include <unordered_map>
#include <iostream>
#include <set>
#include <stdexcept>
#include <limits>
#include <array>
namespace rrl { bool seed_index(BTreeIndex&, const WorkloadTrace&, std::unordered_map<idx_key_t,std::uint64_t>&); }
using namespace rrl;
int main(){
  auto fail=[](bool ok,const char* msg){if(!ok)std::cerr<<"FAIL: "<<msg<<"\n";return ok;};
  WorkloadConfig a; a.kind=WorkloadKind::YcsbA;a.key_count=1000;a.transaction_count=100;a.operations_per_transaction=10;a.zipf_theta=0;a.seed=9;
  auto ta=WorkloadTrace::generate(a); if(!fail(ta.transactions().size()==100,"ycsb txn")||!fail(ta.transactions()[0].operations.size()==10,"ycsb ops"))return 1;
  WorkloadConfig e=a;e.kind=WorkloadKind::YcsbE;e.range_length=8;auto te=WorkloadTrace::generate(e);size_t ranges=0,inserts=0;std::vector<Operation> range_ops,insert_ops;for(auto& t:te.transactions())for(auto& o:t.operations){ranges+=o.kind==OperationKind::RangeRead;inserts+=o.kind==OperationKind::Insert;if(o.kind==OperationKind::RangeRead)range_ops.push_back(o);if(o.kind==OperationKind::Insert)insert_ops.push_back(o);}bool overlap=false;for(const auto& r:range_ops)for(const auto& i:insert_ops)if(i.key>=r.key&&i.key<r.end_key)overlap=true;if(!fail(ranges+inserts==1000,"ycsb e mix")||!fail(!insert_ops.empty(),"ycsb e inserts")||!fail(overlap,"ycsb e phantom overlap")||!fail(te.max_key()>=1000,"range upper bound"))return 1;
  WorkloadConfig p;p.kind=WorkloadKind::TpccPayment;p.key_count=10;p.transaction_count=4;auto tp=WorkloadTrace::generate(p);if(!fail(tp.transactions().size()==4,"payment txn")||!fail(tp.transactions()[0].operations.size()==3,"payment ops"))return 1;
  WorkloadConfig n=p;n.kind=WorkloadKind::TpccNewOrder;n.tpcc_items_per_order=5;auto tn=WorkloadTrace::generate(n);if(!fail(tn.transactions()[0].operations.size()==13,"new order ops"))return 1;
  for (auto kind : {WorkloadKind::YcsbA, WorkloadKind::YcsbB, WorkloadKind::YcsbE, WorkloadKind::YcsbESkew}) {
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
  for(auto kind:{WorkloadKind::YcsbA,WorkloadKind::YcsbB,WorkloadKind::YcsbE,WorkloadKind::YcsbESkew,WorkloadKind::TpccPayment,WorkloadKind::TpccNewOrder}) {
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

  if(!fail(WorkloadTrace::parse_kind("YCSB-E-SKEW")==WorkloadKind::YcsbESkew &&
           WorkloadTrace::parse_kind("e-skew")==WorkloadKind::YcsbESkew &&
           std::string(WorkloadTrace::name(WorkloadKind::YcsbESkew))=="ycsb-e-skew","skew identity"))return 1;
  for(double theta:{0.,.4,.7,.9,.99}) {
    WorkloadConfig c=e;c.request_distribution="zipfian";c.zipf_theta=theta;c.seed=8;c.transaction_count=1000;
    auto old=WorkloadTrace::generate(c);c.kind=WorkloadKind::YcsbESkew;
    auto skew=WorkloadTrace::generate(c);
    for(size_t i=0;i<old.transactions().size();++i) {
      const auto& x=old.transactions()[i].operations;const auto& y=skew.transactions()[i].operations;
      for(size_t j=0;j<x.size();++j)
        if(!fail(x[j].key==y[j].key && x[j].kind==y[j].kind && x[j].end_key==y[j].end_key && x[j].value==y[j].value,
                 "E-skew reuses exact E trace and RNG sequence"))return 1;
    }
  }
  // Both scan and insertion anchors become hot; retain the 95/5 mix.
  std::array<size_t,2> hot_scans{},hot_inserts{};
  for(size_t variant=0;variant<2;++variant) {
    WorkloadConfig c=e;c.kind=WorkloadKind::YcsbESkew;c.request_distribution="zipfian";c.key_count=10000;
    c.transaction_count=5000;c.zipf_theta=variant?.99:0;c.seed=8;c.range_length=32;
    auto trace=WorkloadTrace::generate(c);size_t scans=0,ins=0;
    for(const auto& t:trace.transactions())for(const auto& o:t.operations) {
      if(o.kind==OperationKind::RangeRead) {
        ++scans;hot_scans[variant]+=o.key<c.key_count/10;
        if(!fail(o.key<o.end_key && o.end_key==std::min<idx_key_t>(c.key_count,o.key+32),"clipped half-open key-width scan"))return 1;
      } else if(o.kind==OperationKind::Insert) {++ins;hot_inserts[variant]+=o.key<c.key_count/10;}
      else if(!fail(false,"unexpected E-skew operation"))return 1;
    }
    if(!fail(scans+ins==50000 && ins>2000 && ins<3000,"95/5 operation mix"))return 1;
    if(variant==0 && !fail(hot_scans[0]>4000 && hot_scans[0]<5500,"uniform scan anchor control"))return 1;
  }
  if(!fail(hot_scans[1]>hot_scans[0]*3 && hot_inserts[1]>hot_inserts[0]*3,
           "ordered skew concentrates both scan and insert anchors"))return 1;
  for(double theta:{-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
    WorkloadConfig c=e;c.kind=WorkloadKind::YcsbESkew;c.zipf_theta=theta;
    bool bad=false;try{(void)WorkloadTrace::generate(c);}catch(const std::invalid_argument&){bad=true;}
    if(!fail(bad,"invalid theta rejected by trace API"))return 1;
  }
  for(size_t width:{size_t(0),std::numeric_limits<size_t>::max()}) {
    WorkloadConfig c=e;c.kind=WorkloadKind::YcsbESkew;c.range_length=width;
    bool bad=false;try{(void)WorkloadTrace::generate(c);}catch(const std::invalid_argument&){bad=true;}
    if(!fail(bad,"invalid scan width rejected by trace API"))return 1;
  }

  // Official hotspot model: uniform within low-key hot set / remaining cold set.
  for(double fraction:{.1,.2})for(double probability:{.2,.8,.9}) {
    WorkloadConfig c=e;c.kind=WorkloadKind::YcsbESkew;c.key_count=10000;
    c.transaction_count=5000;c.hotspot_data_fraction=fraction;c.hotspot_opn_fraction=probability;
    c.seed=8;c.request_distribution="hotspot";
    auto trace=WorkloadTrace::generate(c);size_t scans=0,ins=0,hot_scans=0,hot_ins=0;
    const idx_key_t hot=static_cast<idx_key_t>(c.key_count*fraction);
    for(const auto& t:trace.transactions())for(const auto& o:t.operations) {
      if(!fail(o.key<c.key_count,"hotspot anchor in domain"))return 1;
      if(o.kind==OperationKind::RangeRead) {++scans;hot_scans+=o.key<hot;}
      else if(o.kind==OperationKind::Insert) {++ins;hot_ins+=o.key<hot;}
      else if(!fail(false,"hotspot retains E operation mix"))return 1;
    }
    if(!fail(std::abs(double(hot_scans)/scans-probability)<.02 &&
             std::abs(double(hot_ins)/ins-probability)<.05,"scan/insert hotspot probability"))return 1;
  }
  WorkloadConfig default_skew=e;default_skew.kind=WorkloadKind::YcsbESkew;
  if(!fail(std::string(WorkloadTrace::key_distribution(default_skew))=="hotspot" &&
           default_skew.hotspot_data_fraction==.2 && default_skew.hotspot_opn_fraction==.8,
           "E-skew auto defaults to official 80/20 hotspot"))return 1;
  for(double probability:{0.,1.}) {
    WorkloadConfig c=default_skew;c.key_count=100;c.operations_per_transaction=10;
    c.hotspot_data_fraction=probability;
    c.hotspot_opn_fraction=probability;
    if(!fail(!WorkloadTrace::generate(c).transactions().empty(),"valid all-hot/all-cold empty other set"))return 1;
  }
  for(int scenario=0;scenario<7;++scenario) {
    WorkloadConfig c=default_skew;
    if(scenario==0)c.hotspot_data_fraction=-.1;
    if(scenario==1)c.hotspot_opn_fraction=1.1;
    if(scenario==2)c.hotspot_data_fraction=std::numeric_limits<double>::quiet_NaN();
    if(scenario==3)c.hotspot_data_fraction=0; // positive hot probability selects empty set
    if(scenario==4)c.hotspot_data_fraction=1; // positive cold probability selects empty set
    if(scenario==5){c.hotspot_opn_fraction=1;c.hotspot_data_fraction=.001;c.key_count=1000;}
    if(scenario==6)c.request_distribution="not-a-distribution";
    bool bad=false;try{(void)WorkloadTrace::generate(c);}catch(const std::invalid_argument&){bad=true;}
    if(!fail(bad,"invalid hotspot geometry rejected"))return 1;
  }
  std::cout<<"workload trace: OK\n";
}
