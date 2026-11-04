#include "lock/object_lock_manager.h"
#include "lock/native_policy.h"
#include "txn/shared_dataset.h"
#include "rdma/rdma_transport.h"
#include "../src/txn/leaf_hierarchy.h"
#include <cstdlib>
#include <functional>
#include <iostream>
#include <thread>
using namespace rrl;
void require(bool b,const char* m){if(!b)throw std::runtime_error(m);}
TreeSeed fixture_seed(std::size_t keys){TreeSeed seed;for(std::size_t k=0;k<keys;k+=2)seed.push_back({k,k});return seed;}
struct ObservedLocks final:IRangeLockManager {
 ObjectLockManager inner;std::uint64_t leaf=0;bool reject_after_early=false,seen=false;
 std::function<void()> observer;
 RC lock(txnid_t t,LockMode m,const KeyRangeEnt& r,LockKind k,std::vector<txnid_t>* c=nullptr) override {
  if(reject_after_early && seen && k==LockKind::Node && m==LockMode::S)return AbortConflict;
  return inner.lock(t,m,r,k,c);
 }
 RC unlock(txnid_t t,const KeyRangeEnt& r,LockKind k) override {
  auto rc=inner.unlock(t,r,k);
  if(rc==RCOK && k==LockKind::Node && r.StartKey==leaf){seen=true;if(observer)observer();}
  return rc;
 }
 RC unlock_all(txnid_t t) override{return inner.unlock_all(t);}
 std::size_t get_lock_count(txnid_t t=0) const override{return inner.get_lock_count(t);}
};
int main(int argc,char** argv){try {
 WorkloadConfig cfg;RdmaTreeConfig tree;std::uint16_t ignored=0;
 if(argc>1 && std::string(argv[1])=="--config") {
  load_shared_dataset_config(argv[2],cfg,tree,ignored);const auto seed=fixture_seed(cfg.key_count);
  RdmaTreeMemory memory(tree,seed);RdmaListener listener(std::stoi(argv[4]));require(listener.valid(),"fixture listener");
  std::cout<<"RDMA ancestor fixture listening"<<std::endl;
  for(;;){auto c=std::make_unique<RdmaConnection>();require(c->accept_server(listener,memory.data(),memory.bytes()),"fixture accept");std::thread([c=std::move(c)]() mutable {c->wait_for_disconnect();}).detach();}
 }
 const auto* cp=std::getenv("RRL_TEST_CONFIG");if(!cp)return 77;
 load_shared_dataset_config(cp,cfg,tree,ignored);const auto seed=fixture_seed(cfg.key_count);
 const auto* hp=std::getenv("RRL_TEST_RDMA_HOST");const std::string host=hp?hp:"127.0.0.1";
 const auto lp=std::stoi(std::getenv("RRL_TEST_RDMA_PORT")),dp=std::stoi(std::getenv("RRL_TEST_DATA_PORT")),sp=std::stoi(std::getenv("RRL_TEST_TXN_STATE_PORT"));
 BTreeRdma index;require(index.connect(host,dp,1,tree_seed_fingerprint(seed)),"fixture data connect");require(index.init(tree.order)==RCOK,"fixture init");
 index.prepare_thread(0,256);index.prepare_write_capacity(16);
 const auto keys=detail::object_record_keys(WorkloadTrace::generate(cfg));
 ObjectLockManager first,second;require(first.connect(host,lp,keys,1) && second.connect(host,lp,keys,1),"fixture objects");
 NativePolicyClient a(host,sp,first),b(host,sp,second);
 std::vector<IndexNodeSpan> scratch;scratch.reserve(2048);
 std::vector<detail::HierarchyRequest> plan,other,current;for(auto* v:{&plan,&other,&current})v->reserve(4096);
 auto build=[&](const TransactionTrace& tx,auto& target){require(detail::leaf_plan(index,tx,scratch,target),"actual ancestry plan");};
 auto acquire=[&](NativePolicyClient& client,txnid_t id,const auto& requests) {
  for(const auto& r:requests){const auto result=client.acquire(id,r.mode,r.range,r.kind).result;if(result!=TxnResult::Ok)return result;}return TxnResult::Ok;
 };
 auto end=[&](NativePolicyClient& client,ObjectLockManager& locks,txnid_t id){if(client.has_physical_locks())require(locks.unlock_all(id)==RCOK,"release all");require(client.end(id).result==TxnResult::Ok,"end");};
 auto get=[&](idx_key_t key){void* v=nullptr;require(index.lookup(key,v),"lookup");return *static_cast<std::uint64_t*>(v);};
 const std::string mode=argc>1?argv[1]:"strict";
 if(mode=="root") {
  TransactionTrace tx;tx.operations={{OperationKind::Insert,1,0,99}};build(tx,plan);
  require(plan.front().range.StartKey==0 && plan.front().mode==LockMode::X,"only root growth requires logical root X");
  require(index.hierarchy_cover_into(KeyRangeEnt::point(1),scratch),"old root");const auto old=scratch.front().id;
  auto x=a.begin(WaitPolicy::NoWait,0,5000000000ULL);require(x.result==TxnResult::Ok && acquire(a,x.id,plan)==TxnResult::Ok,"root writer");
  require(a.prepare_commit(x.id).result==TxnResult::Ok,"root prepare");std::vector<IndexMutation> changes{{1,&tx.operations[0].value,false}};
  require(index.apply_batch(changes)==RCOK,"root split");require(index.hierarchy_cover_into(KeyRangeEnt::point(1),scratch) && scratch.front().id!=old,"real root replaced");
  TransactionTrace read;read.operations={{OperationKind::Read,1,0,0}};build(read,other);
  auto y=b.begin(WaitPolicy::NoWait,0,5000000000ULL);require(acquire(b,y.id,other)==TxnResult::Conflict,"new root hidden until writer releases");end(b,second,y.id);end(a,first,x.id);
 }else if(mode=="strict") {
  TransactionTrace left,right;left.operations={{OperationKind::Insert,1,0,111}};right.operations={{OperationKind::Insert,cfg.key_count-3,0,222}};
  build(left,plan);build(right,other);require(plan.front().mode==LockMode::IX && other.front().mode==LockMode::IX,"no global topology X");
  auto x=a.begin(WaitPolicy::NoWait,0,5000000000ULL);require(acquire(a,x.id,plan)==TxnResult::Ok,"hold left structural transaction");
  auto y=b.begin(WaitPolicy::NoWait,0,5000000000ULL);require(acquire(b,y.id,other)==TxnResult::Ok,"independent right insertion progresses while left held");
  std::vector<IndexMutation> changes{{right.operations[0].key,&right.operations[0].value,false}};
  require(b.prepare_commit(y.id).result==TxnResult::Ok && index.apply_batch(changes)==RCOK,"right commits first");end(b,second,y.id);
  require(a.prepare_commit(x.id).result==TxnResult::Ok,"left prepare");changes[0]={1,&left.operations[0].value,false};require(index.apply_batch(changes)==RCOK,"left write");end(a,first,x.id);
  const auto target=cfg.key_count-1;
  require(index.hierarchy_cover_into(KeyRangeEnt::point(target),scratch),"split leaf metadata");auto leaf=scratch.back();
  std::vector<std::uint64_t> values(cfg.key_count,77);
  for(auto key=leaf.range.StartKey;key<cfg.key_count && leaf.keys<leaf.max_keys;++key) {
   void* p=nullptr;if(key==target || index.lookup(key,p))continue;
   changes[0]={key,&values[key],false};require(index.apply_batch(changes)==RCOK,"fill split leaf");
   require(index.hierarchy_cover_into(KeyRangeEnt::point(target),scratch),"fill metadata");leaf=scratch.back();
  }
  require(leaf.keys==leaf.max_keys,"leaf actually full");
  TransactionTrace read,write;read.operations={{OperationKind::Read,target,0,0}};write.operations={{OperationKind::Insert,target,0,333}};
  build(read,other);const auto stale=other;build(write,plan);
  x=a.begin(WaitPolicy::NoWait,0,5000000000ULL);require(acquire(a,x.id,plan)==TxnResult::Ok,"split ancestor locks");
  require(a.prepare_commit(x.id).result==TxnResult::Ok,"split prepare");changes[0]={target,&write.operations[0].value,false};require(index.apply_batch(changes)==RCOK,"actual nonroot split");
  require(index.hierarchy_cover_into(KeyRangeEnt::point(target),scratch) && scratch.back().id!=leaf.id,"new right leaf published");
  build(read,current);require(!detail::covers_hierarchy_plan(stale,current),"stale pre-split plan rejected");
  y=b.begin(WaitPolicy::NoWait,0,5000000000ULL);require(acquire(b,y.id,current)==TxnResult::Conflict,"new leaf remains under held safe ancestor");end(b,second,y.id);end(a,first,x.id);
  y=b.begin(WaitPolicy::NoWait,0,5000000000ULL);require(acquire(b,y.id,stale)==TxnResult::Ok,"stale IDs alone could grant");
  build(read,current);require(!detail::covers_hierarchy_plan(stale,current),"post-lock path validation refuses stale authority");end(b,second,y.id);
  y=b.begin(WaitPolicy::NoWait,0,5000000000ULL);require(acquire(b,y.id,current)==TxnResult::Ok && get(target)==333,"new path reads committed value");end(b,second,y.id);
 }else if(mode=="deneva" || mode=="partial" || mode=="split-abort") {
  cfg.kind=WorkloadKind::YcsbE;cfg.threads=1;cfg.transaction_count=1;cfg.operations_per_transaction=mode=="partial"?2:1;cfg.zipf_theta=0;
  idx_key_t target=1;
  std::vector<IndexMutation> changes;changes.reserve(1);std::vector<std::uint64_t> values(cfg.key_count,55);
  if(mode=="split-abort") {
   target=cfg.key_count-1;require(index.hierarchy_cover_into(KeyRangeEnt::point(target),scratch),"abort leaf");auto leaf=scratch.back();
   for(auto key=leaf.range.StartKey;key<cfg.key_count && leaf.keys<leaf.max_keys-1;++key){void* value=nullptr;if(key==target || index.lookup(key,value))continue;changes.clear();changes.push_back({key,&values[key],false});require(index.apply_batch(changes)==RCOK,"fill source abort threshold");require(index.hierarchy_cover_into(KeyRangeEnt::point(target),scratch),"abort threshold metadata");leaf=scratch.back();}
   require(leaf.keys>=leaf.max_keys-1,"source split threshold");
  }
  WorkloadTrace trace;
  for(cfg.seed=1;cfg.seed<1000000;++cfg.seed) {
   trace=WorkloadTrace::generate(cfg);const auto& ops=trace.transactions()[0].operations;
   if(ops[0].kind==OperationKind::Insert && ops[0].key==target && (mode!="partial" || ops[1].kind==OperationKind::RangeRead))break;
  }
  require(cfg.seed<1000000,"source-compatible trace found");
  require(index.hierarchy_cover_into(KeyRangeEnt::point(target),scratch),"early leaf metadata");const auto leaf=scratch.back().id;
  ObservedLocks observed;require(observed.inner.connect(host,lp,keys,1),"observed objects");observed.leaf=leaf;observed.reject_after_early=mode=="partial";
  bool early_visible=false;
  if(mode=="deneva")observed.observer=[&] {
   // This executes inside early unlock, before the original transaction can
   // prepare/end. A distinct native transaction must see the released leaf.
   TransactionTrace read;read.operations={{OperationKind::Read,target,0,0}};build(read,other);
   auto y=b.begin(WaitPolicy::NoWait,0,5000000000ULL);require(acquire(b,y.id,other)==TxnResult::Ok,"source early release permits independent reader");
   require(get(target)==trace.transactions()[0].operations[0].value,"source early write visible before End");end(b,second,y.id);early_visible=true;
  };
  index.finish_thread();
  PolicyRunConfig run;run.protocol=PolicyProtocol::Hierarchy;run.leaf_hierarchy=true;run.deneva_insert_behavior=true;
  run.txn_state_port=sp;run.coordinator_host=host;run.max_attempts=mode=="split-abort"?3:1;
  const auto result=run_policy_workload(index,observed,trace,run);require(!result.failed(),"Deneva behavior hard error");
  index.prepare_thread(0,256);index.prepare_write_capacity(16);
  if(mode=="deneva")require(result.committed==1 && result.policy.deneva_early_inserts==1 && result.policy.deneva_early_unlocks==1 && early_visible,"real early insert and release");
  if(mode=="partial")require(result.committed==0 && result.aborted==1 && result.policy.deneva_partial_write_aborts==1 && get(target)==trace.transactions()[0].operations[0].value,"source early write deliberately retained after later abort");
  if(mode=="split-abort") {
   void* value=nullptr;require(result.committed==0 && result.aborted==3 && result.policy.deneva_split_aborts==3 && !index.lookup(target,value) && !observed.seen,"source early Abort does not execute unreachable split");
  }
 }else throw std::runtime_error("unknown ancestor test");
 require(first.get_lock_count()==0,"cohort released");require(index.validate(),"tree invariants");index.finish_thread();
 std::cout<<"Actual ancestor / source compatibility OK: "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
