#pragma once
#include "lock/lock_manager.h"
#include <limits>
#include <stdexcept>

namespace paper_contract {
using namespace rrl;
inline void require(bool ok,const char* what) {
    if(!ok)throw std::runtime_error(what);
}
// Test the same observable histories against local and live RDMA managers.
// Sources: Eswaran1976 section 3 (13)-(14); Gray1976 Table 1.
// This contract tests the shared mechanism, not full ARIES/KVL or InnoDB.
inline void check(IRangeLockManager& locks) {
    auto clear=[&](txnid_t t){require(locks.unlock_all(t)==RCOK,"contract cleanup");};
    constexpr txnid_t reader=60001,writer=60002;
    const auto max=std::numeric_limits<idx_key_t>::max();
    struct Case {KeyRangeEnt range;idx_key_t inside,outside;};
    const Case cases[]={
        {KeyRangeEnt::half_open(10,20),10,20},
        {KeyRangeEnt::half_open(0,1),0,1},
        {KeyRangeEnt::closed(max-1,max),max,max-2},
        {KeyRangeEnt::to_right(7,Bound::Closed),7,8},
        {KeyRangeEnt::from_left(max,Bound::Closed),max,max-1}
    };
    for(auto kind:{LockKind::Predicate,LockKind::ExactRange})for(const auto& c:cases) {
        // An empty result is still a lock on the absence of matching tuples.
        require(locks.lock(reader,LockMode::S,c.range,kind)==RCOK,"predicate read");
        require(locks.lock(writer,LockMode::S,KeyRangeEnt::point(c.inside),kind)==RCOK,"read/read coexist");
        clear(writer);
        require(locks.lock(writer,LockMode::X,KeyRangeEnt::point(c.inside),kind)==AbortConflict,"phantom insert excluded");
        require(locks.lock(writer,LockMode::X,KeyRangeEnt::point(c.outside),kind)==RCOK,"outside predicate independent");
        clear(writer);
        clear(reader);
        // Writer-first must exclude the reader too, not just read->write.
        require(locks.lock(writer,LockMode::X,KeyRangeEnt::point(c.inside),kind)==RCOK,"writer first");
        require(locks.lock(reader,LockMode::S,c.range,kind)==AbortConflict,"reverse conflict");
        clear(writer);
        require(locks.lock(reader,LockMode::S,c.range,kind)==RCOK,"read after commit");
        clear(reader);
    }
    // Distinct tags cannot turn two equivalent predicates into isolated tables.
    for(bool reverse:{false,true}) {
        auto a=reverse?LockKind::ExactRange:LockKind::Predicate;
        auto b=reverse?LockKind::Predicate:LockKind::ExactRange;
        require(locks.lock(reader,LockMode::S,KeyRangeEnt::half_open(10,20),a)==RCOK,"cross-tag reader");
        require(locks.lock(writer,LockMode::X,KeyRangeEnt::point(15),b)==AbortConflict,"cross-tag safety");
        clear(reader);
        require(locks.lock(writer,LockMode::X,KeyRangeEnt::point(15),b)==RCOK,"cross-tag after release");
        clear(writer);
    }
    // Gray Table 1 restricted to the four currently supported modes.
    const LockMode modes[]={LockMode::IS,LockMode::IX,LockMode::S,LockMode::X};
    const bool compatible[4][4]={
        {true,true,true,false},{true,true,false,false},
        {true,false,true,false},{false,false,false,false}
    };
    for(unsigned a=0;a<4;++a)for(unsigned b=0;b<4;++b) {
        const auto node=KeyRangeEnt::closed(64,127);
        require(locks.lock(reader,modes[a],node,LockKind::Node)==RCOK,"MGL first");
        const auto result=locks.lock(writer,modes[b],node,LockKind::Node);
        require(result==(compatible[a][b]?RCOK:AbortConflict),"Gray compatibility");
        clear(writer);clear(reader);
    }
    // Intention alone is not a subtree S lock. Reader IS must coexist
    // with writer IX; an actual covering S is what excludes that writer.
    const auto root=KeyRangeEnt::unbounded(),bucket=KeyRangeEnt::closed(64,127);
    require(locks.lock(reader,LockMode::IS,root,LockKind::Node)==RCOK,"reader ancestor IS");
    require(locks.lock(writer,LockMode::IX,root,LockKind::Node)==RCOK,"writer ancestor IX coexists");
    require(locks.lock(reader,LockMode::S,bucket,LockKind::Node)==RCOK,"cover subtree with S");
    require(locks.lock(writer,LockMode::IX,bucket,LockKind::Node)==AbortConflict,"S vs IX excludes child write");
    clear(reader);clear(writer);
    // Record-only negative control: locking existing keys does not cover a gap.
    require(locks.lock(reader,LockMode::S,KeyRangeEnt::point(10),LockKind::Record)==RCOK,"record left");
    require(locks.lock(reader,LockMode::S,KeyRangeEnt::point(20),LockKind::Record)==RCOK,"record right");
    require(locks.lock(writer,LockMode::X,KeyRangeEnt::point(15),LockKind::Record)==RCOK,"record phantom control");
    clear(reader);clear(writer);
    require(locks.get_lock_count()==0,"paper contract leaves no locks");
}
} // namespace paper_contract
