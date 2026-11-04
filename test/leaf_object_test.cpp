#include "lock/object_lock_manager.h"
#include "lock/native_policy.h"
#include "txn/shared_dataset.h"
#include "../src/txn/leaf_hierarchy.h"
#include <iostream>
#include <cstdlib>
#include <set>
using namespace rrl;
void require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
struct PreloadedTree final:BTreeIndex {
 BTreeLatch tree;
 RC init(std::size_t n) override{return tree.init(n);}
 RC insert(idx_key_t k,void* v) override{return tree.insert(k,v);}
 RC erase(idx_key_t k) override{return tree.erase(k);}
 bool lookup(idx_key_t k,void*& v) const override{return tree.lookup(k,v);}
 std::vector<KeyValue> scan_range(const KeyRangeEnt& r) const override{return tree.scan_range(r);}
 std::size_t size() const override{return tree.size();}
 bool preloaded() const override{return true;}
 RC apply_batch(const std::vector<IndexMutation>& w) override{return tree.apply_batch(w);}
 bool leaf_cover_into(const KeyRangeEnt& r,std::vector<LeafSpan>& out) const override{return tree.leaf_cover_into(r,out);}
};
void missing_update_test() {
 WorkloadConfig cfg;cfg.key_count=16;cfg.transaction_count=1;cfg.operations_per_transaction=1;cfg.threads=1;
 auto trace=WorkloadTrace::generate(cfg);
 while(trace.transactions()[0].operations[0].kind!=OperationKind::Update){++cfg.seed;trace=WorkloadTrace::generate(cfg);}
 const auto op=trace.transactions()[0].operations[0];
 PreloadedTree index;require(index.init(4)==RCOK,"missing update tree");
 std::vector<std::uint64_t> values(16);
 for(std::size_t k=0;k<16;++k)if(k!=op.key){values[k]=k;require(index.insert(k,&values[k])==RCOK,"missing update seed");}
 ObjectLockManager locks;require(locks.init_local(detail::object_record_keys(trace),256,1,128),"missing update objects");
 PolicyRunConfig run;run.protocol=PolicyProtocol::Hierarchy;run.leaf_hierarchy=true;run.wait_policy=WaitPolicy::WaitDie;
 const auto result=run_policy_workload(index,locks,trace,run);
 require(!result.failed() && result.committed==1 && result.aborted==1 && result.policy.retries==1,"absent update restarts before structural mutation");
 void* value=nullptr;require(index.lookup(op.key,value) && *static_cast<std::uint64_t*>(value)==op.value,"missing update applied under X topology");
 require(index.tree.validate() && locks.get_lock_count()==0,"missing update invariants");
}
int main(int argc,char** argv){try {
 missing_update_test();
 const bool remote=argc>1 && std::string(argv[1])=="--remote";
 WorkloadConfig c;c.key_count=128;c.transaction_count=100;c.operations_per_transaction=4;c.zipf_theta=0;
 const auto trace=WorkloadTrace::generate(c);const auto keys=detail::object_record_keys(trace);
 BTreeLatch local;BTreeRdma rdma;BTreeIndex* index=&local;
 ObjectLockManager locks;
 if(remote) {
  const auto* hp=std::getenv("RRL_TEST_RDMA_HOST");const std::string host=hp?hp:"127.0.0.1";
  const auto* cp=std::getenv("RRL_TEST_CONFIG");WorkloadConfig dataset;RdmaTreeConfig tree;std::uint16_t ignored;
  load_shared_dataset_config(cp,dataset,tree,ignored);
  require(locks.connect(host,std::stoi(std::getenv("RRL_TEST_RDMA_PORT")),detail::object_record_keys(WorkloadTrace::generate(dataset)),2),"object connect");
  require(rdma.connect(host,std::stoi(std::getenv("RRL_TEST_DATA_PORT")),1,tree_seed_fingerprint(make_shared_seed(WorkloadTrace::generate(dataset)))),"data connect");
  require(rdma.init(tree.order)==RCOK,"remote init");rdma.prepare_thread(0,256);rdma.prepare_write_capacity(4);index=&rdma;
 }else {
  require(locks.init_local(keys,8192,2,4096),"local object init");require(local.init(8)==RCOK,"init");
 }
 std::vector<std::uint64_t> values(128);if(!remote)for(std::size_t k=0;k<128;++k){values[k]=k;require(index->insert(k,&values[k])==RCOK,"seed");}
 std::vector<LeafSpan> leaves;leaves.reserve(256);require(index->leaf_cover_into(KeyRangeEnt::unbounded(),leaves),"actual leaf snapshot");
 require(leaves.size()>1,"actual tree leaves");std::set<std::uint64_t> identities;
 for(std::size_t n=0;n<leaves.size();++n) {
  require(leaves[n].id>=2 && identities.insert(leaves[n].id).second,"real unique leaf identities");
  if(n)require(leaves[n-1].range.EndKey==leaves[n].range.StartKey,"contiguous routing fences");
 }
 const auto first=leaves[0],other=leaves[1];
 require(locks.lock(1,LockMode::S,KeyRangeEnt::point(first.id),LockKind::Node)==RCOK,"scan leaf S");
 std::vector<txnid_t> conflicts;conflicts.reserve(16);
 require(locks.lock(2,LockMode::IX,KeyRangeEnt::point(first.id),LockKind::Node,&conflicts)==AbortConflict && conflicts==std::vector<txnid_t>{1},"same actual leaf writer conflict and owner");
 require(locks.lock(2,LockMode::IX,KeyRangeEnt::point(other.id),LockKind::Node)==RCOK,"other leaf progresses");
 require(locks.lock(2,LockMode::X,KeyRangeEnt::point(first.id),LockKind::Record)==RCOK,"record and leaf namespaces separate");
 require(locks.unlock_all(2)==RCOK && locks.unlock_all(1)==RCOK && locks.get_lock_count()==0,"strict release");
 require(locks.lock(3,LockMode::S,KeyRangeEnt::point(1),LockKind::Node)==RCOK,"stable topology shared");
 require(locks.lock(4,LockMode::X,KeyRangeEnt::point(1),LockKind::Node)==AbortConflict,"insert/split excluded until scan ends");
 require(locks.unlock_all(3)==RCOK,"topology release");
 require(locks.lock(4,LockMode::X,KeyRangeEnt::point(1),LockKind::Node)==RCOK,"split can proceed after lease");
 require(locks.unlock_all(4)==RCOK,"split release");
 std::vector<detail::HierarchyRequest> plan;plan.reserve(512);std::vector<LeafSpan> scratch;scratch.reserve(256);
 TransactionTrace mixed;mixed.operations={{OperationKind::RangeRead,first.range.StartKey,first.range.EndKey,0},{OperationKind::Update,first.range.StartKey,0,9}};
 require(detail::leaf_plan(*index,mixed,scratch,plan),"actual leaf mixed plan");
 auto found=std::find_if(plan.begin(),plan.end(),[&](const auto& r){return r.kind==LockKind::Node && r.range.StartKey==first.id;});
 require(found!=plan.end() && found->mode==LockMode::X,"scan/write final X with no upgrade");
 std::vector<LeafSpan> point;point.reserve(8);require(index->leaf_cover_into(KeyRangeEnt::point(UINT64_MAX),point) && point.size()==1 && point[0].id==leaves.back().id,"max key belongs to real last leaf");
 index->finish_thread();if(remote)require(rdma.validate(),"remote invariants");else require(local.validate(),"local invariants");
 std::cout<<"Actual leaf IDs/fences, final modes, per-object authority, namespaces and topology fencing OK\n";
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
