// SPDX-License-Identifier: GPL-3.0-only
// Derived from iotlpf/ROLEX 016a397 rolex/leaf.hpp. Local adapter helpers:
// explicit occupancy supports UINT64_MAX (upstream reserves it as sentinel).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>
namespace rolex {
template<std::size_t N=64,typename K=std::uint64_t,typename V=std::uint64_t>
struct Leaf {
    K keys[N]{};
    V vals[N]{};
    std::size_t used_slots{0};
    static constexpr std::size_t max_slot(){return N;}
    static constexpr K invalidKey(){return std::numeric_limits<K>::max();}
    bool isfull() const{return used_slots==N;}
    bool isEmpty() const{return used_slots==0;}
    std::size_t count() const{return used_slots;}
    K first_key() const{return keys[0];}
    K last_valid_key() const{return used_slots?keys[used_slots-1]:invalidKey();}
    V load_value(std::size_t i) const{return __atomic_load_n(&vals[i],__ATOMIC_RELAXED);}
    void store_value(std::size_t i,V value){__atomic_store_n(&vals[i],value,__ATOMIC_RELAXED);}
    bool search(K key,V& value) const {
        const auto* it=std::lower_bound(keys,keys+used_slots,key);
        if(it==keys+used_slots || *it!=key)return false;
        value=load_value(static_cast<std::size_t>(it-keys));return true;
    }
    bool contain(K key) const{V value{};return search(key,value);}
    std::size_t insert_not_full(K key,V value) {
        const auto i=static_cast<std::size_t>(std::lower_bound(keys,keys+used_slots,key)-keys);
        if(i<used_slots && keys[i]==key){store_value(i,value);return i;}
        if(isfull())return N;
        std::copy_backward(keys+i,keys+used_slots,keys+used_slots+1);
        std::copy_backward(vals+i,vals+used_slots,vals+used_slots+1);
        keys[i]=key;vals[i]=value;++used_slots;return i;
    }
    bool remove(K key) {
        const auto i=static_cast<std::size_t>(std::lower_bound(keys,keys+used_slots,key)-keys);
        if(i==used_slots || keys[i]!=key)return false;
        std::copy(keys+i+1,keys+used_slots,keys+i);
        std::copy(vals+i+1,vals+used_slots,vals+i);--used_slots;return true;
    }
    void range_kv(K start,bool has_end,K end,bool inclusive,std::vector<std::pair<K,V>>& out) const {
        auto i=static_cast<std::size_t>(std::lower_bound(keys,keys+used_slots,start)-keys);
        for(;i<used_slots && (!has_end || keys[i]<end || (inclusive && keys[i]==end));++i)
            out.emplace_back(keys[i],load_value(i));
    }
};
}
