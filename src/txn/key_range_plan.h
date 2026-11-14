#pragma once
#include "txn/workload.h"
#include <stdexcept>
namespace rrl::detail {
// Graefe2010 section 5.1: neighboring physical keys define next/prior units.
// Stable numeric predicates retain their original coverage through SMOs;
// no successor-key lock-name migration can silently shrink a held lock.
struct KeyRequest {LockMode mode;KeyRangeEnt range;LockKind kind;};
struct KeyObservation {
    IndexKeyNeighbors lower,upper;
    std::vector<idx_key_t> keys;
};
inline bool same_neighbors(const IndexKeyNeighbors& a,const IndexKeyNeighbors& b) {
    return a.present==b.present && a.has_predecessor==b.has_predecessor &&
        a.has_successor==b.has_successor &&
        (!a.has_predecessor || a.predecessor==b.predecessor) &&
        (!a.has_successor || a.successor==b.successor);
}
inline bool same_observation(const KeyObservation& a,const KeyObservation& b,bool scan) {
    return same_neighbors(a.lower,b.lower) && (!scan ||
        (same_neighbors(a.upper,b.upper) && a.keys==b.keys));
}
// Compact next/prior scan locks are a continuous union determined only by
// the boundary neighbors. Interior keys cannot change that union. Gap planning
// still enumerates physical records and therefore uses the default full path.
inline bool observe_keys(BTreeIndex& tree,const Operation& op,KeyObservation& out,bool enumerate_range=true) {
    if(op.kind==OperationKind::RangeRead && op.key>=op.end_key) {
        if(op.key>op.end_key)throw std::invalid_argument("reversed key range");
        out={};return true;
    }
    if(!tree.key_neighbors(op.key,out.lower))throw std::runtime_error("index has no key-neighbor metadata capability");
    if(op.kind!=OperationKind::RangeRead)return true;
    if(!tree.key_neighbors(op.end_key,out.upper))
        throw std::runtime_error("index has no key-neighbor metadata capability");
    if(!enumerate_range){out.keys.clear();return true;}
    if(!tree.key_range_into(KeyRangeEnt::half_open(op.key,op.end_key),out.keys))
        throw std::runtime_error("index has no key-range metadata capability");
    // A mutation between the three short snapshots is a normal replan abort.
    auto first=out.lower.present?op.key:out.lower.successor;
    const bool first_inside=out.lower.present ||
        (out.lower.has_successor && out.lower.successor<op.end_key);
    if(first_inside!=( !out.keys.empty()))return false;
    if(first_inside && out.keys.front()!=first)return false;
    const bool last_inside=out.upper.has_predecessor && out.upper.predecessor>=op.key;
    if(last_inside!=(!out.keys.empty()))return false;
    if(last_inside && out.keys.back()!=out.upper.predecessor)return false;
    return true;
}
inline void key_plan(const Operation& op,const KeyObservation& s,PolicyProtocol protocol,
                     std::vector<KeyRequest>& plan) {
    plan.clear();const bool scan=op.kind==OperationKind::RangeRead;
    if(scan && op.key>=op.end_key)return;
    const bool read=op.kind==OperationKind::Read || scan;
    const auto kind=protocol==PolicyProtocol::PriorKey?LockKind::PriorKey:LockKind::NextKey;
    auto add=[&](LockMode mode,KeyRangeEnt range,LockKind k) {
        if(range.valid() && !range.empty())plan.push_back({mode,range,k});
    };
    const auto query=scan?KeyRangeEnt::half_open(op.key,op.end_key):KeyRangeEnt::point(op.key);
    auto interval=[&](bool has_left,idx_key_t left,bool has_right,idx_key_t right,Bound lb,Bound rb) {
        KeyRangeEnt range;range.has_start=has_left;range.StartKey=left;range.start_bound=lb;
        range.has_end=has_right;range.EndKey=right;range.end_bound=rb;return range;
    };
    const auto& n=s.lower;
    if(!scan) {
        if(protocol==PolicyProtocol::Gap) {
            const auto gap=interval(n.has_predecessor,n.predecessor,n.has_successor,n.successor,Bound::Open,Bound::Open);
            if(read && !n.present)add(LockMode::S,gap,LockKind::Gap);
            if(!read && op.kind!=OperationKind::Delete && (op.kind==OperationKind::Insert || !n.present))add(LockMode::X,KeyRangeEnt::point(op.key),LockKind::InsertIntent);
            add(read?LockMode::S:LockMode::X,KeyRangeEnt::point(op.key),LockKind::Record);
        } else if(protocol==PolicyProtocol::NextKey) {
            add(read?LockMode::S:LockMode::X,
                interval(n.has_predecessor,n.predecessor,n.present || n.has_successor,
                         n.present?op.key:n.successor,Bound::Open,Bound::Closed),kind);
            if(op.kind==OperationKind::Delete && n.present)
                add(LockMode::X,interval(true,op.key,n.has_successor,n.successor,Bound::Open,Bound::Closed),kind);
        } else {
            add(read?LockMode::S:LockMode::X,
                interval(n.present || n.has_predecessor,n.present?op.key:n.predecessor,
                         n.has_successor,n.successor,Bound::Closed,Bound::Open),kind);
            if(op.kind==OperationKind::Delete && n.present)
                add(LockMode::X,interval(n.has_predecessor,n.predecessor,true,op.key,Bound::Closed,Bound::Open),kind);
        }
        return;
    }
    bool has_left=n.has_predecessor;auto left=n.predecessor;
    const auto& u=s.upper;
    const bool has_right=u.present || u.has_successor;
    const auto right=u.present?op.end_key:u.successor;
    if(protocol==PolicyProtocol::PriorKey && n.present){has_left=true;left=op.key;}
    for(auto key:s.keys) {
        if(protocol==PolicyProtocol::PriorKey && key==op.key)continue;
        const auto range=interval(has_left,left,true,key,
            protocol==PolicyProtocol::PriorKey?Bound::Closed:Bound::Open,
            protocol==PolicyProtocol::NextKey?Bound::Closed:Bound::Open);
        if(protocol!=PolicyProtocol::Gap || range.overlaps(query))
            add(LockMode::S,range,protocol==PolicyProtocol::Gap?LockKind::Gap:kind);
        has_left=true;left=key;
    }
    const auto tail=interval(has_left,left,has_right,right,
        protocol==PolicyProtocol::PriorKey?Bound::Closed:Bound::Open,
        protocol==PolicyProtocol::NextKey?Bound::Closed:Bound::Open);
    if(protocol!=PolicyProtocol::Gap || tail.overlaps(query))
        add(LockMode::S,tail,protocol==PolicyProtocol::Gap?LockKind::Gap:kind);
    // Retained ghost keys can be revived without changing physical metadata.
    // Lock them too, not just rows returned by a live-record scan.
    if(protocol==PolicyProtocol::Gap)for(auto key:s.keys)
        add(LockMode::S,KeyRangeEnt::point(key),LockKind::Record);
}
} // namespace rrl::detail
