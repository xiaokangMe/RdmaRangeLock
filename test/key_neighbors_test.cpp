#include "index/btree.h"
#include <map>
#include <random>
#include <iostream>
#include <limits>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
using namespace rrl;
template<class Tree> void check_tree() {
    Tree tree;CHECK(tree.init(4)==RCOK);
    std::map<idx_key_t,std::uint64_t> model;
    std::mt19937_64 rng(987);
    auto check=[&](idx_key_t key) {
        IndexKeyNeighbors n;CHECK(tree.key_neighbors(key,n));
        auto at=model.lower_bound(key);
        CHECK(n.present==(at!=model.end() && at->first==key));
        CHECK(n.has_predecessor==(at!=model.begin()));
        if(n.has_predecessor)CHECK(n.predecessor==std::prev(at)->first);
        auto after=at;if(n.present)++after;
        CHECK(n.has_successor==(after!=model.end()));
        if(n.has_successor)CHECK(n.successor==after->first);
    };
    check(0);check(UINT64_MAX);
    for(unsigned step=0;step<600;++step) {
        const auto key=step%31==0?UINT64_MAX:rng()%160;
        if(step%4==0){tree.erase(key);model.erase(key);}
        else {model[key]=step;CHECK(tree.insert(key,&model[key])==RCOK);}
        check(key);check(0);check(UINT64_MAX);check(rng()%161);
        if(!tree.validate())throw std::runtime_error(std::string(typeid(Tree).name())+" invalid at step="+std::to_string(step)+" key="+std::to_string(key));
        std::vector<idx_key_t> keys;CHECK(tree.key_range_into(KeyRangeEnt::half_open(30,90),keys));
        std::vector<idx_key_t> want;
        for(auto p:model)if(p.first>=30 && p.first<90)want.push_back(p.first);
        CHECK(keys==want);
    }
}
int main(){try{check_tree<BTreeSingle>();check_tree<BTreeLatch>();
    std::cout<<"Neighbor metadata: empty, cross-leaf, splits, physical deletion, uint64 bounds OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
