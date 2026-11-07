#include "index/rdma_btree.h"
#include "../src/index/rdma_btree_core.h"
#include <iostream>
#include <map>
#include <random>
#include <set>
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);} while(0)
struct Memory : rrl::detail::TreeIO {
    char* p;std::size_t bytes;bool fail{false};
    Memory(void* data,std::size_t n):p(static_cast<char*>(data)),bytes(n){}
    bool read(std::uint64_t off,void* out,std::size_t n) override {
        if(fail || off>bytes || n>bytes-off)return false;std::memcpy(out,p+off,n);return true;
    }
    bool write(std::uint64_t off,const void* in,std::size_t n) override {
        if(fail || off>bytes || n>bytes-off)return false;std::memcpy(p+off,in,n);return true;
    }
};
int main() {try {
    using namespace rrl;
    for(auto order:{3U,4U,16U,128U}) {
        RdmaTreeMemory image(RdmaTreeConfig{order,16384,4096,1000},{});
        Memory memory(image.data(),image.bytes());OffsetBTreeHeader h;
        CHECK(memory.read(0,&h,sizeof(h)));CHECK(detail::valid_header(h,image.bytes()));
        detail::TreeCore tree(memory,h);std::map<idx_key_t,std::uint64_t> reference;
        std::mt19937_64 rng(71);std::set<idx_key_t> physical;
        for(unsigned step=0;step<12000;++step) {
            auto key=step%97==0?UINT64_MAX:rng()%2048;auto value=rng();bool remove=rng()%4==0;
            std::vector<IndexMutation> changes{{key,&value,remove}};
            CHECK(tree.capacity(changes));tree.mutate(changes[0]);tree.publish();
            if(remove)reference.erase(key);else {reference[key]=value;physical.insert(key);}
            IndexKeyNeighbors neighbors;tree.key_neighbors(key,neighbors);
            auto at=physical.lower_bound(key);
            CHECK(neighbors.present==(at!=physical.end() && *at==key));
            CHECK(neighbors.has_predecessor==(at!=physical.begin()));
            if(neighbors.has_predecessor)CHECK(neighbors.predecessor==*std::prev(at));
            auto after=at;if(neighbors.present)++after;
            CHECK(neighbors.has_successor==(after!=physical.end()));
            if(neighbors.has_successor)CHECK(neighbors.successor==*after);
            if(step%251==0) {
                std::vector<idx_key_t> keys;tree.key_range(KeyRangeEnt::unbounded(),keys);
                CHECK(keys==std::vector<idx_key_t>(physical.begin(),physical.end()));
            }
            std::uint64_t got=0;CHECK(tree.lookup(key,got)==bool(reference.count(key)));
            if(reference.count(key))CHECK(got==reference.at(key));
            if(step%251==0) {
                std::vector<KeyValue> rows;rows.reserve(4096);std::vector<std::uint64_t> values(4096);
                tree.scan(KeyRangeEnt::unbounded(),rows,values);CHECK(rows.size()==reference.size());
                std::size_t n=0;for(const auto& p:reference){CHECK(rows[n].key==p.first);CHECK(*static_cast<std::uint64_t*>(rows[n++].value)==p.second);}
                CHECK(tree.h.live_records==reference.size());
            }
        }
        // Empty/open/single-key and unsigned upper boundary.
        std::vector<KeyValue> rows;rows.reserve(4096);std::vector<std::uint64_t> values(4096);
        tree.scan(KeyRangeEnt::open(4,4),rows,values);CHECK(rows.empty());
        tree.scan(KeyRangeEnt::point(UINT64_MAX),rows,values);CHECK(rows.size()==reference.count(UINT64_MAX));
        memory.fail=true;bool failed=false;try{std::uint64_t v;tree.lookup(0,v);}catch(const detail::TreeFault&){failed=true;}CHECK(failed);
    }
    RdmaTreeMemory tiny(RdmaTreeConfig{3,1,1,1000},{});Memory memory(tiny.data(),tiny.bytes());
    OffsetBTreeHeader h;memory.read(0,&h,sizeof(h));detail::TreeCore tree(memory,h);
    std::uint64_t v=10;std::vector<IndexMutation> batch{{1,&v,false},{2,&v,false}};
    CHECK(!tree.capacity(batch));CHECK(tree.h.live_records==0 && tree.h.records_used==0);
    h.root=h.bytes;CHECK(!detail::valid_header(h,tiny.bytes()));
    std::cout<<"Offset tree splits, records, tombstones, ranges, bounds and fault propagation: OK\n";
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
