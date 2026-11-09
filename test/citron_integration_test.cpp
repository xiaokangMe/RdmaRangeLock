#include <algorithm>
#include "citron/citron_lock_manager.h"
#include "citron/backends.h"
#include <iostream>
#include <random>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdlib>
using namespace rrl;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
class FaultBackend final:public citron::Backend {
public:
  citron::LocalBackend local;bool fail{false};
  explicit FaultBackend(std::vector<std::uint64_t>& words):local(words.data(),words.size()/citron::kNodeWords){}
  bool read_node(std::uint64_t i,citron::Node& n)override{return local.read_node(i,n);}
  bool cas(std::uint64_t i,int f,std::uint64_t c,std::uint64_t s,std::uint64_t& p)override{
    const bool ok=local.cas(i,f,c,s,p);
    if(fail && f==citron::kBitmapW){fail=false;return false;}return ok;
  }
  bool faa(std::uint64_t i,int f,std::uint64_t a,std::uint64_t& p)override{return local.faa(i,f,a,p);}
  std::uint64_t operations()const override{return local.operations();}
};
int main(int argc,char** argv)try {
  citron::LockManagerConfig cfg;cfg.tree_height=3;cfg.max_sessions=16;cfg.lock_budget=256;
  cfg.client.t_wait_ns=1000000;cfg.client.lock_deadline_ns=50000000;
  if(const auto* host=std::getenv("RRL_TEST_CITRON_HOST"))cfg.server_host=host;
  if(const auto* port=std::getenv("RRL_TEST_CITRON_PORT"))cfg.server_port=std::stoi(port);
  citron::CitronLockManager locks(cfg);
  auto request=[&](txnid_t id,LockMode mode,std::uint64_t a,std::uint64_t b){return locks.lock(id,mode,KeyRangeEnt::half_open(a,b));};
  require(request(1,LockMode::S,5,15)==RCOK,"reader maps to exclusive");
  require(request(2,LockMode::S,6,7)==Abort,"shared overlap conservatively conflicts");
  require(request(1,LockMode::X,5,15)==RCOK,"self repeated acquisition");
  require(request(1,LockMode::X,3,22)==RCOK,"self partial extension");
  require(request(2,LockMode::X,21,22)==Abort,"extended range excludes others");
  require(request(2,LockMode::X,23,24)==RCOK,"disjoint bits share leaf");
  require(locks.unlock_all(1)==RCOK && locks.unlock_all(2)==RCOK,"release all");
  require(request(1,LockMode::X,5,6)==RCOK,"point before ancestor");
  require(request(1,LockMode::X,0,1024)==RCOK,"ancestor extension avoids self ticket");
  require(request(2,LockMode::X,1023,1024)==Abort,"extended ancestor protected");
  require(locks.unlock_all(1)==RCOK && locks.unlock_all(2)==RCOK,"extension cleanup");
  require(locks.get_lock_count()==0,"no held locks after cleanup");
  require(locks.lock(1,LockMode::X,KeyRangeEnt::point(UINT64_MAX))==ERROR,"overflow fails closed");
  require(locks.lock(1,LockMode::IS,KeyRangeEnt::point(1))==ERROR,"unsupported mode rejected");
  require(locks.lock(1,LockMode::S,KeyRangeEnt::point(1),LockKind::Gap)==ERROR,"unsupported kind rejected");
  // Two live requests with conflicting logical ranges can NEVER be granted.
  const int seconds=argc>1?std::stoi(argv[1]):2;
  const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
  std::mutex mutex;std::vector<KeyRangeEnt> active;std::atomic<unsigned> violations{0},errors{0},grants{0};
  std::vector<std::thread> threads;
  for(unsigned w=0;w<8;++w)threads.emplace_back([&,w] {
    std::mt19937_64 random(w+17);
    while(std::chrono::steady_clock::now()<end) {
      const auto a=random()%4096;auto range=KeyRangeEnt::half_open(a,std::min<std::uint64_t>(4096,a+1+random()%512));
      const auto rc=locks.lock(w+100,LockMode::X,range);
      if(rc==RCOK) {
        {std::lock_guard<std::mutex> guard(mutex);for(const auto& prior:active)if(prior.overlaps(range))++violations;active.push_back(range);}
        std::this_thread::yield();
        {std::lock_guard<std::mutex> guard(mutex);auto it=std::find_if(active.begin(),active.end(),[&](const auto& x){return x.StartKey==range.StartKey && x.EndKey==range.EndKey;});if(it!=active.end())active.erase(it);else ++errors;}
        ++grants;
      }else if(rc!=Abort && rc!=AbortCapacity)++errors;
      if(locks.unlock_all(w+100)!=RCOK)++errors;
    }
  });
  for(auto& t:threads)t.join();
  require(violations==0 && errors==0 && grants>0,"random ancestor/descendant mutual exclusion");
  require(locks.get_lock_count()==0,"stress lock leak");
  if(cfg.server_host.empty()) {
    citron::SegmentTree tree(2);std::vector<std::uint64_t> words((tree.total_nodes()+2)*citron::kNodeWords);
    auto backend=std::make_unique<FaultBackend>(words);auto* fault=backend.get();
    citron::CitronConfig config;config.abort_on_conflict=true;config.t_wait_ns=1000000;
    citron::CitronClient first(std::move(backend),tree,config);citron::RangeLock held,other;
    require(first.acquire(1,2,true,held)==citron::CitronClient::Status::Acquired,"fault setup");
    fault->fail=true;
    require(!first.release(held) && !held.nodes.empty(),"uncertain release retains diagnostic ownership");
    citron::CitronClient second(std::make_unique<citron::LocalBackend>(words.data(),tree.total_nodes()+2),tree,config);
    require(second.acquire(5,6,true,other)==citron::CitronClient::Status::Error,"shared poison rejects new grants");
    require(!first.release(held),"uncertain FAA/CAS never replayed");
  }
  std::cout<<"Citron integration PASS grants="<<grants<<" violations="<<violations<<" errors="<<errors<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
