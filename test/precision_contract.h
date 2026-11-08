#pragma once
#include "lock/lock_manager.h"
#include <stdexcept>
#include <vector>

namespace rrl::precision_contract {
inline void require(bool value,const char* what) {if(!value)throw std::runtime_error(what);}
inline void check(IRangeLockManager& locks) {
    const txnid_t a=67001,b=67002;
    const auto point=KeyRangeEnt::point(15);
    for(bool griffin:{false,true}) {
        const auto p=griffin?LockKind::GriffinPredicate:LockKind::PrecisionPredicate;
        const auto u=griffin?LockKind::GriffinUpdate:LockKind::PrecisionUpdate;
        const auto release=[&]{require(locks.unlock_all(a)==RCOK,"clear a");require(locks.unlock_all(b)==RCOK,"clear b");};
        std::vector<KeyRangeEnt> ranges{KeyRangeEnt::half_open(10,20),KeyRangeEnt::point(15),
            KeyRangeEnt::unbounded(),KeyRangeEnt::from_left(15),KeyRangeEnt::to_right(15,Bound::Closed)};
        for(const auto& range:ranges) {
            require(locks.lock(a,LockMode::S,range,p)==RCOK,"post Lp before scan");
            require(locks.lock(b,LockMode::S,range,p)==RCOK,"Lp/Lp never conflicts");release();
            require(locks.lock(a,LockMode::S,range,p)==RCOK,"read first");
            require(locks.lock(b,LockMode::X,point,u)==AbortConflict,"Lp prevents update");release();
            require(locks.lock(a,LockMode::X,point,u)==RCOK,"write first");
            require(locks.lock(b,LockMode::S,range,p)==AbortConflict,"Lu prevents predicate read");release();
        }
        require(locks.lock(a,LockMode::X,point,u)==RCOK,"Lu first");
        require(locks.lock(b,LockMode::X,point,u)==RCOK,"Lu/Lu deliberately not checked");release();
        // Jordan §4.1: unique inserts, delete and modifications implicitly read.
        require(locks.lock(a,LockMode::S,point,p)==RCOK,"first implicit read");
        require(locks.lock(a,LockMode::X,point,u)==RCOK,"own Lp does not block Lu");
        require(locks.lock(b,LockMode::S,point,p)==AbortConflict,"implicit read prevents lost update/duplicate");
        require(locks.get_lock_count(a)==2,"Lp and Lu distinct entries");
        release();
        require(locks.lock(a,LockMode::S,KeyRangeEnt::half_open(10,20),p)==RCOK,"half-open");
        require(locks.lock(b,LockMode::X,KeyRangeEnt::point(20),u)==RCOK,"end excluded");release();
        require(locks.lock(a,LockMode::S,KeyRangeEnt::point(UINT64_MAX),p)==RCOK,"max key");
        require(locks.lock(b,LockMode::X,KeyRangeEnt::point(UINT64_MAX),u)==AbortConflict,"max conflict");release();
        require(locks.lock(a,LockMode::S,KeyRangeEnt::point(0),p)==RCOK,"zero key");
        require(locks.lock(b,LockMode::X,KeyRangeEnt::point(0),u)==AbortConflict,"zero conflict");release();
        require(locks.lock(a,LockMode::X,KeyRangeEnt::half_open(10,20),u)==ERROR,"Lu cannot be range");
        require(locks.lock(a,LockMode::X,point,p)==ERROR,"Lp is read only");
        require(locks.lock(a,LockMode::S,point,u)==ERROR,"Lu is write only");
        release();
    }
    // Membership and record locks have deliberately distinct responsibilities.
    require(locks.lock(a,LockMode::S,KeyRangeEnt::half_open(10,20),LockKind::GriffinPredicate)==RCOK,"Griffin scan");
    require(locks.lock(b,LockMode::X,point,LockKind::Record)==RCOK,"value update alone has no phantom");
    require(locks.lock(b,LockMode::X,point,LockKind::GriffinUpdate)==AbortConflict,"membership update excluded");
    require(locks.unlock_all(b)==RCOK,"clear b");
    require(locks.lock(a,LockMode::S,point,LockKind::Record)==RCOK,"protect scanned value");
    require(locks.lock(b,LockMode::X,point,LockKind::Record)==AbortConflict,"record 2PL excludes lost update");
    require(locks.unlock_all(a)==RCOK && locks.unlock_all(b)==RCOK,"final cleanup");
}
} // namespace rrl::precision_contract
