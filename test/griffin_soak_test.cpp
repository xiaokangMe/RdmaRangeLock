// Long-lived structural correctness stress, not a performance benchmark.
#include "index/griffin.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <set>
#include <thread>
#include <vector>
using namespace rrl;
int main(int argc,char** argv) {try {
    const unsigned seconds=argc>1?static_cast<unsigned>(std::stoul(argv[1])):2;
    if(!seconds || seconds>3600)throw std::invalid_argument("seconds must be 1..3600");
    constexpr unsigned threads=32,keys_per_thread=512;
    BTreeGriffin tree; if(tree.init(8)!=RCOK)throw std::runtime_error("init");
    std::vector<std::uint64_t> values(threads*keys_per_thread);
    std::vector<unsigned char> present(values.size(),0);
    for(std::size_t k=0;k<values.size();++k)values[k]=k;
    std::atomic<bool> error{false};
    std::atomic<std::uint64_t> mutations{0},scans{0},pending{0},capacity{0};
    const auto start=std::chrono::steady_clock::now(),end=start+std::chrono::seconds(seconds);
    std::vector<std::thread> workers;
    for(unsigned t=0;t<threads;++t)workers.emplace_back([&,t] {
        try {
            tree.prepare_thread(t,values.size());
            std::vector<IndexMutation> batch(1);
            for(std::uint64_t n=0;!error && std::chrono::steady_clock::now()<end;++n) {
                const auto k=t*keys_per_thread+(n%keys_per_thread);
                batch[0]={k,&values[k],present[k]!=0};
                const auto rc=tree.apply_batch(batch);
                if(rc==RCOK){present[k]=!present[k];++mutations;}
                else if(rc==AbortCapacity)++capacity;
                else throw std::runtime_error("mutation failed");
                if(n%8==0) {
                    try {
                        const auto rows=tree.scan_range(KeyRangeEnt::half_open(k,k+32));
                        idx_key_t prior=0;bool first=true;
                        for(const auto& row:rows) {
                            if(row.key>=values.size() || row.value!=&values[row.key] ||
                               (!first && prior>=row.key))throw std::runtime_error("invalid scan");
                            prior=row.key;first=false;
                        }
                        ++scans;
                    }catch(const IndexSyncPending&){++pending;}
                }
            }
        }catch(...){error=true;}
    });
    for(auto& w:workers)w.join();
    if(error || !tree.healthy())throw std::runtime_error("worker failure");
    tree.wait_synced();
    if(!tree.validate())throw std::runtime_error("structure invalid");
    const auto rows=tree.scan_range(KeyRangeEnt::unbounded());
    std::set<idx_key_t> actual,expected;
    for(std::size_t k=0;k<present.size();++k)if(present[k])expected.insert(k);
    for(const auto& row:rows) {
        if(row.value!=&values.at(row.key))throw std::runtime_error("wrong final value");
        actual.insert(row.key);
    }
    if(actual!=expected || !mutations || scans+pending==0)throw std::runtime_error("final model mismatch/no progress");
    std::cout<<"PASS duration_seconds="<<seconds<<" threads="<<threads
             <<" mutations="<<mutations<<" scans="<<scans<<" pending_aborts="<<pending
             <<" capacity_aborts="<<capacity<<" final_keys="<<actual.size()<<"\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
