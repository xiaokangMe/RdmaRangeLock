#include "txn/shared_dataset.h"
#include "common/execution_context.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
using namespace rrl;
namespace {std::atomic<bool> paused{false},release_writer{false};
void pause_store(){paused=true;while(!release_writer)cooperative_yield();}
void require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
}
namespace rrl {struct BTreeRdmaTestPeer {static void pause(BTreeRdma& tree){tree.before_value_store_test_hook_=pause_store;}};}
int main(){try {
 const auto* dp=std::getenv("RRL_TEST_DATA_PORT");if(!dp)return 77;
 const auto* hp=std::getenv("RRL_TEST_RDMA_HOST");const std::string host=hp?hp:"127.0.0.1";
 WorkloadConfig cfg;RdmaTreeConfig tc;std::uint16_t port;
 load_shared_dataset_config("experiments/configs/rdma_shared_smoke.json",cfg,tc,port);
 const auto fingerprint=tree_seed_fingerprint(make_shared_seed(WorkloadTrace::generate(cfg)));
 BTreeRdma first,second;require(first.connect(host,std::stoi(dp),1,fingerprint) && second.connect(host,std::stoi(dp),1,fingerprint),"connections");
 require(first.init(tc.order)==RCOK && second.init(tc.order)==RCOK,"init");
 // Use two pre-existing live rows; create these outside the measured barrier.
 first.prepare_thread(0,8);first.prepare_write_capacity(4);
 std::uint64_t a=1,b=2;const auto ka=UINT64_MAX-4096,kb=ka+1;
 require(first.insert(ka,&a)==RCOK && first.insert(kb,&b)==RCOK,"seed two rows");first.finish_thread();
 BTreeRdmaTestPeer::pause(first);std::atomic<bool> error{false},other_done{false};
 std::thread writer([&]{try{first.prepare_thread(0,8);first.prepare_write_capacity(4);a=11;require(first.apply_batch({{ka,&a,false}})==RCOK,"paused update");first.finish_thread();}catch(...){error=true;release_writer=true;}});
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 while(!paused && !error && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
 std::thread unrelated([&]{try{second.prepare_thread(0,8);second.prepare_write_capacity(4);b=22;require(second.apply_batch({{kb,&b,false}})==RCOK,"independent update");second.finish_thread();other_done=true;}catch(...){error=true;}});
 const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 while(!other_done && !error && std::chrono::steady_clock::now()<end)std::this_thread::yield();
 const bool progressed=paused && other_done && !release_writer;
 release_writer=true;writer.join();unrelated.join();
 require(progressed && !error,"independent update must complete while first writer is paused under its record latch");
 second.prepare_thread(0,8);void* value=nullptr;
 require(second.lookup(ka,value) && *static_cast<std::uint64_t*>(value)==11,"first real writeback");
 require(second.lookup(kb,value) && *static_cast<std::uint64_t*>(value)==22,"second real writeback");
 second.finish_thread();require(first.validate(),"post-update invariants");
 std::cout<<"Independent real RDMA writers overlap; correct writeback and record latches OK\n";
}catch(const std::exception& e){release_writer=true;std::cerr<<e.what()<<'\n';return 1;}}
