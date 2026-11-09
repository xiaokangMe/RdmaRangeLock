#include "citron/backends.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace citron {

LocalBackend::LocalBackend(std::uint64_t* words, std::size_t node_count)
    : words_(words), node_count_(node_count) {
    if (!words_ || node_count_ < 2) throw std::invalid_argument("invalid local region");
}

std::uint64_t* LocalBackend::word(std::uint64_t index, int field) const {
    return &words_[(index - 1) * kNodeWords + static_cast<std::size_t>(field)];
}

bool LocalBackend::read_node(std::uint64_t index, Node& out) {
    if (index == 0 || index > node_count_) return false;
    ++operations_;
    for (int i = 0; i < kNodeWords; ++i)
        out.words[i] = __atomic_load_n(word(index, i), __ATOMIC_SEQ_CST);
    return true;
}

bool LocalBackend::cas(std::uint64_t index, int field, std::uint64_t expected,
                       std::uint64_t desired, std::uint64_t& previous) {
    if (index == 0 || index > node_count_ || field < 0 || field >= kNodeWords)
        return false;
    ++operations_;
    previous = expected;
    __atomic_compare_exchange_n(word(index, field), &previous, desired, false,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return true;
}

bool LocalBackend::faa(std::uint64_t index, int field, std::uint64_t add,
                       std::uint64_t& previous) {
    if (index == 0 || index > node_count_ || field < 0 || field >= kNodeWords)
        return false;
    ++operations_;
    previous = __atomic_fetch_add(word(index, field), add, __ATOMIC_SEQ_CST);
    return true;
}

RdmaBackend::RdmaBackend(const std::string& host, std::uint16_t port) {
    if (!connection_.connect_client(host, port))
        throw std::runtime_error("citron RDMA connect/handshake failed");
}

std::uint64_t RdmaBackend::offset(std::uint64_t index, int field) const {
    return (index - 1) * kNodeBytes + static_cast<std::uint64_t>(field) * 8;
}

bool RdmaBackend::read_node(std::uint64_t index, Node& out) {
    const auto& m=connection_.remote_memory();
    if(!index || index>m.length/kNodeBytes)return false;
    return connection_.read(m.address+offset(index,0),m.rkey,&out,sizeof(out));
}
bool RdmaBackend::read_node_many(const std::vector<std::uint64_t>& ids, std::vector<Node>& out) {
    const auto& m=connection_.remote_memory();out.resize(ids.size());
    for(std::size_t first=0;first<ids.size();first+=128) {
        std::vector<rrl::RdmaReadRequest> requests;
        for(std::size_t i=first;i<std::min(first+128,ids.size());++i) {
            if(!ids[i] || ids[i]>m.length/kNodeBytes)return false;
            requests.push_back({m.address+offset(ids[i],0),m.rkey,&out[i],sizeof(Node)});
        }
        if(!connection_.read_batch(requests))return false;
    }
    return true;
}
bool RdmaBackend::read_node_range(std::uint64_t first,std::size_t count,std::vector<Node>& out) {
    const auto& m=connection_.remote_memory();
    if(!count){out.clear();return true;}
    if(!first || first>m.length/kNodeBytes || count>m.length/kNodeBytes-first+1)return false;
    out.resize(count);
    // Keep each transfer within the common transport's scratch capacity.
    for(std::size_t i=0;i<count;i+=128) {
        const auto n=std::min<std::size_t>(128,count-i);
        if(!connection_.read(m.address+offset(first+i,0),m.rkey,out.data()+i,n*sizeof(Node)))return false;
    }
    return true;
}
bool RdmaBackend::cas(std::uint64_t index,int field,std::uint64_t expected,std::uint64_t desired,std::uint64_t& prior) {
    const auto& m=connection_.remote_memory();
    if(!index || index>m.length/kNodeBytes || field<0 || field>=kNodeWords)return false;
    return connection_.compare_swap(m.address+offset(index,field),m.rkey,expected,desired,prior);
}
bool RdmaBackend::faa(std::uint64_t index,int field,std::uint64_t add,std::uint64_t& prior) {
    const auto& m=connection_.remote_memory();
    if(!index || index>m.length/kNodeBytes || field<0 || field>=kNodeWords)return false;
    return connection_.fetch_add(m.address+offset(index,field),m.rkey,add,prior);
}
bool RdmaBackend::faa_many(const std::vector<FaaOp>& ops) {
    // Sequential verbs retain the exact successful-prefix information needed
    // by rollback. Each is a native FAA through the common transport.
    return Backend::faa_many(ops);
}
} // namespace citron
