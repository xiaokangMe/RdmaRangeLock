#pragma once
#include "lock/lock_manager.h"
#include <stdexcept>
namespace granularity_contract {
using namespace rrl;
inline void require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
inline void check(IRangeLockManager& locks) {
    auto* conversion=dynamic_cast<ILockModeConversion*>(&locks);require(conversion,"conversion capability");
    constexpr txnid_t a=64001,b=64002;
    const auto bucket=KeyRangeEnt::closed(0,255),root=KeyRangeEnt::unbounded();
    auto clear=[&]{require(locks.unlock_all(a)==RCOK && locks.unlock_all(b)==RCOK,"conversion cleanup");};
    auto get=[&](txnid_t t,LockMode m,KeyRangeEnt r,LockKind k){return locks.lock(t,m,r,k);};
    require(get(a,LockMode::IX,root,LockKind::Node)==RCOK && get(a,LockMode::IX,bucket,LockKind::Node)==RCOK,"fine ancestors");
    require(get(a,LockMode::X,KeyRangeEnt::point(10),LockKind::Record)==RCOK &&
            get(a,LockMode::X,KeyRangeEnt::point(20),LockKind::Record)==RCOK,"fine history");
    require(get(b,LockMode::IS,bucket,LockKind::Node)==RCOK &&
            get(b,LockMode::S,KeyRangeEnt::point(11),LockKind::Record)==RCOK,"peer at distinct record");
    require(conversion->convert_lock(a,LockMode::X,bucket,LockKind::Node)==AbortConflict,"escalation can fail");
    require(get(b,LockMode::IX,bucket,LockKind::Node)==RCOK,"failed escalation retained IX, not X");
    require(locks.unlock_all(b)==RCOK,"peer commits");
    require(conversion->convert_lock(a,LockMode::X,bucket,LockKind::Node)==RCOK,"publish cover before removing fine locks");
    require(locks.unlock(a,KeyRangeEnt::point(10),LockKind::Record)==RCOK &&
            locks.unlock(a,KeyRangeEnt::point(20),LockKind::Record)==RCOK,"remove redundant fine representation");
    require(get(b,LockMode::X,KeyRangeEnt::point(10),LockKind::Record)==AbortConflict,"history covered after escalation");
    require(get(a,LockMode::X,KeyRangeEnt::point(10),LockKind::Record)==RCOK &&
            get(a,LockMode::X,KeyRangeEnt::point(20),LockKind::Record)==RCOK,"restore complete fine history before weakening");
    require(conversion->convert_lock(a,LockMode::IX,bucket,LockKind::Node)==RCOK,"de-escalate atomically");
    require(get(b,LockMode::IX,bucket,LockKind::Node)==RCOK &&
            get(b,LockMode::X,KeyRangeEnt::point(11),LockKind::Record)==RCOK,"extra overlocking released");
    require(get(b,LockMode::X,KeyRangeEnt::point(10),LockKind::Record)==AbortConflict,"write history remains protected");
    require(get(a,LockMode::S,bucket,LockKind::Node)==AbortConflict,"cache cannot retain pre-downgrade X privilege");
    clear();
    require(get(a,LockMode::IS,root,LockKind::Node)==RCOK && get(a,LockMode::S,bucket,LockKind::Node)==RCOK,"scan cover");
    require(get(a,LockMode::S,KeyRangeEnt::half_open(12,18),LockKind::HierarchyPredicate)==RCOK,"restore exact scan including empty gaps");
    require(conversion->convert_lock(a,LockMode::IS,bucket,LockKind::Node)==RCOK,"scan de-escalation");
    require(get(b,LockMode::IX,root,LockKind::Node)==RCOK &&
            get(b,LockMode::IX,bucket,LockKind::Node)==RCOK,"intent does not conflict with fine descendant predicate");
    require(get(b,LockMode::X,KeyRangeEnt::point(15),LockKind::Record)==AbortConflict,"phantom protection continuous");
    require(get(b,LockMode::X,KeyRangeEnt::point(19),LockKind::Record)==RCOK,"outside query becomes writable");
    clear();require(locks.get_lock_count()==0,"conversion no leaks");
}
}
