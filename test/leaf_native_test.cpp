#include "lock/object_lock_manager.h"
#include "rdma/rdma_transport.h"
#include "lock/native_policy.h"
#include "txn/shared_dataset.h"
#include "../src/txn/leaf_hierarchy.h"
#include "../src/lock/object_lock_layout.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
using namespace rrl;
void require(bool b,const char* m){if(!b)throw std::runtime_error(m);}
int main(int argc,char** argv){try {
 const auto* cp=std::getenv("RRL_TEST_CONFIG");if(!cp)return 77;
 const auto* hp=std::getenv("RRL_TEST_RDMA_HOST");const std::string host=hp?hp:"127.0.0.1";
 const auto lp=std::stoi(std::getenv("RRL_TEST_RDMA_PORT")),dp=std::stoi(std::getenv("RRL_TEST_DATA_PORT")),sp=std::stoi(std::getenv("RRL_TEST_TXN_STATE_PORT"));
 WorkloadConfig cfg;RdmaTreeConfig tree;std::uint16_t port;
 load_shared_dataset_config(cp,cfg,tree,port);auto trace=WorkloadTrace::generate(cfg);const auto keys=detail::object_record_keys(trace);
 const std::string mode=argc>1?argv[1]:"increment";
 if(mode.rfind("fault-",0)==0) {
  RdmaConnection data;require(data.connect_client(host,dp),"data QP");auto mem=data.remote_memory();OffsetBTreeHeader header;
  auto victim=std::make_unique<ObjectLockManager>();ObjectLockManager contender;
  require(victim->connect(host,lp,keys,1) && contender.connect(host,lp,keys,1),"object QPs");
  NativePolicyClient client(host,sp,*victim);const auto b=client.begin(WaitPolicy::WoundWait,0,10000000000ULL);
  require(b.result==TxnResult::Ok,"fault begin");const auto range=KeyRangeEnt::point(0);
  require(client.acquire(b.id,LockMode::X,range,LockKind::Record).result==TxnResult::Ok,"fault victim lock");
  if(mode=="fault-clean" || mode=="fault-committing")require(client.prepare_commit(b.id).result==TxnResult::Ok,"fault prepare");
  if(mode=="fault-clean") {
   require(victim->unlock_all(b.id)==RCOK && client.end(b.id).result==TxnResult::Ok,"clean strict release");
  }
  if(mode=="fault-gate") {
   using namespace object_detail;
   RdmaConnection probe;require(probe.connect_client(host,lp),"gate probe");const auto m=probe.remote_memory();Header h;
   require(probe.read(m.address,m.rkey,&h,sizeof(h)),"gate header");std::uint64_t slot=h.allocated,prior;
   require(slot<h.sessions && probe.compare_swap(m.address+offsetof(Header,allocated),m.rkey,slot,slot+1,prior) && prior==slot,"gate reserve");
   RdmaConnection orphan;require(orphan.connect_client(host,lp,slot+1),"orphan QP");
   object_detail::Session session{1,0};
   require(orphan.write(m.address+h.session_base+slot*sizeof(session),m.rkey,&session,sizeof(session)),"orphan active");
   require(orphan.compare_swap(m.address+h.object_base,m.rkey,0,slot+1,prior) && !prior,"hold object gate");
   require(!orphan.read(m.address,m.rkey^0x80000000u,&prior,8),"failed orphan WC");
  }else victim.reset(); // Only lock QP disappears; state and data stay live.
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  for(;;) {
   const auto rc=contender.lock(b.id+1,LockMode::X,range,LockKind::Record);
   if(mode=="fault-clean") {require(rc==RCOK,"clean service alive");require(contender.unlock_all(b.id+1)==RCOK,"contender release");break;}
   require(rc!=RCOK,"no grant over disconnected owner");if(rc==ERROR)break;
   require(rc==AbortConflict && std::chrono::steady_clock::now()<deadline,"failure fence deadline");std::this_thread::yield();
  }
  if(mode!="fault-clean")require(client.check(b.id).result==TxnResult::Ok,"state QP alive");
  require(data.read(mem.address,mem.rkey,&header,sizeof(header)) && !header.poison,"data QP alive");
  std::cout<<"Object lock-only disconnect fenced: "<<mode<<'\n';return 0;
 }
 BTreeRdma index;require(index.connect(host,dp,1,tree_seed_fingerprint(make_shared_seed(trace))),"data connect");
 require(index.init(tree.order)==RCOK,"tree init");index.prepare_thread(0,256);index.prepare_write_capacity(2);
 auto get=[&](idx_key_t k){void* value=nullptr;require(index.lookup(k,value),"record exists");return *static_cast<std::uint64_t*>(value);};
 if(mode=="read") {
  const auto expected=std::stoull(argv[2]);require(get(0)==expected && get(1)==expected+1,"two-host exact sum");
  ObjectLockManager locks;require(locks.connect(host,lp,keys,1) && locks.get_lock_count()==0,"cohort physical holders released");
 }else {
  ObjectLockManager locks;require(locks.connect(host,lp,keys,1),"objects connect");NativePolicyClient client(host,sp,locks);
  const auto policy=argc>2?parse_wait_policy(argv[2]):WaitPolicy::WoundWait;
  const auto count=argc>3?std::stoull(argv[3]):50;
  std::vector<LeafSpan> scratch;scratch.reserve(256);std::vector<detail::HierarchyRequest> plan;plan.reserve(512);
  std::vector<IndexMutation> pending;pending.reserve(2);
  TransactionTrace tx;tx.operations={{OperationKind::Update,0,0,0},{OperationKind::Update,1,0,0}};
  for(std::uint64_t n=0;n<count;++n) {
   bool done=false;std::uint64_t age=0;
   for(unsigned retry=0;retry<100000 && !done;++retry) {
    const auto b=client.begin(policy,age,5000000000ULL);require(b.result==TxnResult::Ok,"begin");age=b.age;
    auto result=client.acquire(b.id,LockMode::IX,KeyRangeEnt::point(0),LockKind::Node).result;
    if(result==TxnResult::Ok)result=client.acquire(b.id,LockMode::S,KeyRangeEnt::point(1),LockKind::Node).result;
    if(result==TxnResult::Ok) {
     require(detail::leaf_plan(index,tx,scratch,plan),"real leaf plan");
     for(const auto& r:plan){result=client.acquire(b.id,r.mode,r.range,r.kind).result;if(result!=TxnResult::Ok)break;}
    }
    if(result==TxnResult::Ok) {
     auto a=get(0),bval=get(1);require(bval==a+1,"strict two-record state");++a;++bval;
     result=client.prepare_commit(b.id).result;
     if(result==TxnResult::Ok) {
      pending.clear();pending.push_back({0,&a,false});pending.push_back({1,&bval,false});
      require(index.apply_batch(pending)==RCOK,"writeback before release");done=true;
     }
    }
    require(result!=TxnResult::Error,"native object hard error");
    if(client.has_physical_locks())require(locks.unlock_all(b.id)==RCOK,"strict physical release");
    require(client.end(b.id).result==TxnResult::Ok,"state end");
   }
   require(done,"retry exhaustion");
  }

 }
 require(index.validate(),"tree invariants");index.finish_thread();
 std::cout<<"Actual-leaf native two-record atomicity / exact sum OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
