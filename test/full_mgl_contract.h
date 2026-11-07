#pragma once
#include "lock/lock_manager.h"
#include <stdexcept>
namespace full_mgl_contract {
using namespace rrl;
inline void require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
inline void check(IRangeLockManager& locks) {
    // Transcribed independently from the official SQL Server full chart.
    // Order preserves legacy wire values: IS IX S X SIX U IU SIU UIX.
    const char* rows[]={"YYY-YYYYY","YY----Y--","Y-Y--YYY-","---------",
                        "Y-----Y--","Y-Y------","YYY-Y-YY-","Y-Y---YY-","Y--------"};
    const auto object=KeyRangeEnt::point(1);
    auto mode=[](unsigned i){return static_cast<LockMode>(i);};
    auto compatible=[&](unsigned a,unsigned b){return rows[a][b]=='Y';};
    auto clear=[&]{require(locks.unlock_all(63001)==RCOK && locks.unlock_all(63002)==RCOK,"MGL cleanup");};
    for(unsigned a=0;a<9;++a)for(unsigned b=0;b<9;++b) {
        require(compatible(a,b)==compatible(b,a),"symmetric reference table");
        require(lock_modes_conflict(mode(a),mode(b))!=compatible(a,b),"full compatibility matrix");
        require(locks.lock(63001,mode(a),object,LockKind::Node)==RCOK,"MGL mode first");
        require(locks.lock(63002,mode(b),object,LockKind::Node)==(compatible(a,b)?RCOK:AbortConflict),"backend full mode matrix");clear();
        auto joined=lock_mode_join(mode(a),mode(b));auto j=static_cast<unsigned>(joined);
        require(j<9 && joined==lock_mode_join(mode(b),mode(a)),"join commutativity");
        require(lock_mode_join(joined,mode(a))==joined && lock_mode_join(joined,mode(b))==joined,"join covers both modes");
        for(unsigned c=0;c<9;++c) {
            require(lock_mode_join(joined,mode(c))==lock_mode_join(mode(a),lock_mode_join(mode(b),mode(c))),"join associativity");
            require(compatible(j,c)==(compatible(a,c)&&compatible(b,c)),"conversion cannot invent compatibility");
            if(!compatible(a,c))continue;
            require(locks.lock(63001,mode(a),object,LockKind::Node)==RCOK,"conversion original");
            require(locks.lock(63002,mode(c),object,LockKind::Node)==RCOK,"conversion peer");
            require(locks.lock(63001,mode(b),object,LockKind::Node)==(compatible(j,c)?RCOK:AbortConflict),"effective conversion checked before publication");
            require(locks.get_lock_count(63001)==1,"conversion retains one original object");clear();
        }
    }
    require(lock_mode_join(LockMode::S,LockMode::IX)==LockMode::SIX,"S+IX=SIX");
    require(lock_mode_join(LockMode::S,LockMode::IU)==LockMode::SIU,"S+IU=SIU");
    require(lock_mode_join(LockMode::U,LockMode::IX)==LockMode::UIX,"U+IX=UIX");
}
}
