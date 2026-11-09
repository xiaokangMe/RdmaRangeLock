#include "index/rolex.h"
#include <iostream>
#include <map>
using namespace rrl;
void require(bool b,const char* what){if(!b)throw std::runtime_error(what);}
int main()try {
  rolex::port::RolexIndex index(4,64,false,64);require(index.init(64)==RCOK,"init");
  void* ptr=nullptr;require(!index.lookup(42,ptr),"build empty index");
  std::map<std::uint64_t,std::uint64_t> values{{0,10},{1,11},{64,12},{100,13},{UINT64_MAX,14}};
  for(auto& kv:values)require(index.insert(kv.first,&kv.second)==RCOK,"insert after empty build/full key domain");
  require(index.validate() && index.size()==values.size(),"size and structure");
  for(const auto key:std::vector<std::uint64_t>{0,1,2,63,64,99,100,101,UINT64_MAX}) {
    IndexKeyNeighbors got;require(index.key_neighbors(key,got),"neighbor API");
    const auto lower=values.lower_bound(key),upper=values.upper_bound(key);
    require(got.present==(values.count(key)!=0),"present flag");
    require(got.has_predecessor==(lower!=values.begin()),"predecessor flag");
    if(got.has_predecessor)require(got.predecessor==std::prev(lower)->first,"predecessor value");
    require(got.has_successor==(upper!=values.end()),"successor flag");
    if(got.has_successor)require(got.successor==upper->first,"successor value");
  }
  std::vector<idx_key_t> keys;require(index.key_range_into(KeyRangeEnt::closed(100,UINT64_MAX),keys) && keys==std::vector<idx_key_t>({100,UINT64_MAX}),"max key scan");
  std::uint64_t replacement=999;
  require(index.apply_batch({{0,&replacement,false},{1,nullptr,false}})==ERROR,"reject bad batch");
  require(index.lookup(0,ptr) && *static_cast<std::uint64_t*>(ptr)==10,"invalid batch has no written prefix");
  require(index.apply_batch({{1,nullptr,true},{1,&replacement,false}})==RCOK,"delete then reinsert batch");
  require(index.lookup(1,ptr) && *static_cast<std::uint64_t*>(ptr)==999 && index.validate(),"batch result");
  // Exhausted arena must reject a mixed batch before the value update.
  rolex::port::RolexIndex small(4,64,false,64,1);require(small.init(64)==RCOK,"small init");
  std::vector<std::uint64_t> seed(64);
  for(unsigned i=0;i<64;++i)require(small.insert(i*2,&seed[i])==RCOK,"small seed");
  small.prepare_thread(0,10);
  require(small.apply_batch({{0,&replacement,false},{1,&replacement,false}})==ERROR,"capacity precheck");
  require(small.lookup(0,ptr) && ptr==&seed[0] && small.validate(),"capacity failure has no prefix");
  std::cout<<"ROLEX neighbor/full-key-domain/batch contract PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
