#include "index/rolex.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>
using namespace rrl;
int main(int argc,char** argv)try {
  const auto seconds=argc>1?std::stoul(argv[1]):2;
  constexpr std::size_t n=65536;
  std::vector<std::uint64_t> values(n);for(std::size_t i=0;i<n;++i)values[i]=i;
  rolex::port::RolexIndex index(16,64,false,512,8192);
  if(index.init(64)!=RCOK)return 1;
  for(std::size_t i=0;i<n;i+=2)if(index.insert(i,&values[i])!=RCOK)return 1;
  index.prepare_thread(0,16);
  std::atomic<unsigned> errors{0};std::atomic<std::uint64_t> operations{0};
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
  std::vector<std::thread> workers;
  for(std::size_t w=0;w<8;++w)workers.emplace_back([&,w]{
    std::mt19937_64 random(w+1);std::uint64_t ops=0;
    while(std::chrono::steady_clock::now()<deadline) {
      const auto key=(random()%(n/8))*8+w;
      if(index.insert(key,&values[key])!=RCOK)++errors;
      void* value=nullptr;if(!index.lookup(key,value) || value!=&values[key])++errors;
      const auto rows=index.scan_range(KeyRangeEnt::half_open(key,std::min<std::uint64_t>(n,key+16)));
      for(std::size_t i=0;i<rows.size();++i) {
        if((i && rows[i-1].key>=rows[i].key) || rows[i].key<key || rows[i].key>=key+16 ||
           rows[i].value!=&values[rows[i].key])++errors;
      }
      ops+=3;
    }
    operations+=ops;
  });
  for(auto& w:workers)w.join();
  const bool valid=index.validate();
  std::cout<<"ROLEX soak seconds="<<seconds<<" operations="<<operations<<" errors="<<errors<<" valid="<<valid<<std::endl;
  return errors || !valid || !operations?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
