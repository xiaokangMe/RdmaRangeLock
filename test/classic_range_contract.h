#pragma once
#include "../src/txn/key_range_plan.h"
#include <stdexcept>
namespace classic_contract {
using namespace rrl;
inline void require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
inline std::vector<detail::KeyRequest> plan(BTreeIndex& tree,PolicyProtocol p,Operation op) {
    detail::KeyObservation s;require(detail::observe_keys(tree,op,s),"stable metadata");
    std::vector<detail::KeyRequest> result;detail::key_plan(op,s,p,result);return result;
}
inline RC acquire(IRangeLockManager& locks,txnid_t id,const std::vector<detail::KeyRequest>& requests) {
    for(const auto& r:requests){auto rc=locks.lock(id,r.mode,r.range,r.kind);if(rc!=RCOK)return rc;}return RCOK;
}
inline void check(IRangeLockManager& locks) {
    const auto max=std::numeric_limits<idx_key_t>::max();
    constexpr txnid_t a=61001,b=61002;
    auto clear=[&]{require(locks.unlock_all(a)==RCOK && locks.unlock_all(b)==RCOK,"classic cleanup");};
    for(auto p:{PolicyProtocol::Gap,PolicyProtocol::NextKey,PolicyProtocol::PriorKey}) {
        BTreeLatch tree;require(tree.init(4)==RCOK,"classic tree");std::uint64_t value=7;
        for(auto k:{10ULL,20ULL,30ULL})require(tree.insert(k,&value)==RCOK,"seed neighbors");
        auto read=plan(tree,p,{OperationKind::RangeRead,12,18,0});
        require(read.size()==1,"one actual neighboring unit");
        const char* expected=p==PolicyProtocol::Gap?"(10,20)":p==PolicyProtocol::NextKey?"(10,20]":"[10,20)";
        require(read[0].range.to_string()==expected,"actual neighbor boundaries, not query endpoints");
        require(acquire(locks,a,read)==RCOK,"classic reader");
        require(acquire(locks,b,plan(tree,p,{OperationKind::Insert,15,0,1}))==AbortConflict,"phantom excluded");
        require(locks.unlock_all(b)==RCOK,"partial writer cleanup");
        const auto existing=p==PolicyProtocol::PriorKey?10:20;
        const auto update=acquire(locks,b,plan(tree,p,{OperationKind::Update,static_cast<idx_key_t>(existing),0,1}));
        require(update==(p==PolicyProtocol::Gap?RCOK:AbortConflict),"key half and gap-only distinction");
        clear();
        // Writer-first must exclude the same reader history.
        require(acquire(locks,b,plan(tree,p,{OperationKind::Insert,15,0,1}))==RCOK,"writer first");
        require(acquire(locks,a,read)==AbortConflict,"reverse phantom conflict");clear();
        auto empty=plan(tree,p,{OperationKind::RangeRead,20,20,0});
        require(empty.empty(),"empty scan has no phantom predicate");
        for(auto key:{idx_key_t{0},max}) {
            require(acquire(locks,a,plan(tree,p,{OperationKind::Read,key,0,0}))==RCOK,"missing point read");
            require(acquire(locks,b,plan(tree,p,{OperationKind::Insert,key,0,1}))==AbortConflict,"head/tail absence excludes insert");clear();
        }
        require(tree.insert(max,&value)==RCOK,"max is ordinary key");
        require(acquire(locks,a,plan(tree,p,{OperationKind::Read,max,0,0}))==RCOK,"max read");
        require(acquire(locks,b,plan(tree,p,{OperationKind::Update,max,0,1}))==AbortConflict,"max record protection");clear();
        require(acquire(locks,a,plan(tree,p,{OperationKind::RangeRead,10,30,0}))==RCOK,"multi-key scan");
        for(auto key:{10ULL,15ULL,20ULL,29ULL}) {
            require(acquire(locks,b,plan(tree,p,{OperationKind::Insert,key,0,1}))==AbortConflict,"scan records and gaps protected");
            require(locks.unlock_all(b)==RCOK,"writer retry cleanup");
        }
        clear();
        if(p==PolicyProtocol::Gap) {
            require(acquire(locks,a,read)==RCOK,"persistent original gap");
            require(acquire(locks,b,plan(tree,p,{OperationKind::Delete,10,0,0}))==RCOK,"outside boundary delete");
            require(tree.erase(10)==RCOK && locks.unlock_all(b)==RCOK,"boundary deletion commits");
            require(acquire(locks,b,plan(tree,p,{OperationKind::Insert,15,0,1}))==AbortConflict,"numeric guard survives boundary deletion");clear();
        } else {
            const auto deletion=plan(tree,p,{OperationKind::Delete,20,0,0});
            require(deletion.size()==2,"delete protects both adjacent units");
            require(deletion[0].range.to_string()==(p==PolicyProtocol::NextKey?"(10,20]":"[20,30)"),"delete own unit");
            require(deletion[1].range.to_string()==(p==PolicyProtocol::NextKey?"(20,30]":"[10,20)"),"delete merged adjacent unit");
        }
        // Empty index: explicit infinite endpoints, never max as sentinel.
        BTreeLatch blank;require(blank.init(4)==RCOK,"empty tree");
        auto absent=plan(blank,p,{OperationKind::Read,max,0,0});
        require(acquire(locks,a,absent)==RCOK,"empty tree predicate");
        require(acquire(locks,b,plan(blank,p,{OperationKind::Insert,0,0,1}))==AbortConflict,"empty tree phantom");clear();
        // Force splits and verify the protected set is still a logical range.
        for(idx_key_t k=1;k<40;++k)require(tree.insert(k,&value)==RCOK,"split seed");
        require(tree.validate(),"split structure");
        require(acquire(locks,a,plan(tree,p,{OperationKind::RangeRead,12,18,0}))==RCOK,"scan over split leaves");
        require(acquire(locks,b,plan(tree,p,{OperationKind::Delete,15,0,0}))==AbortConflict,"delete inside scan excluded");clear();
    }
    // Pure gap S/X guards coexist, insertion positions remain independent.
    require(locks.lock(a,LockMode::X,KeyRangeEnt::open(10,20),LockKind::Gap)==RCOK,"exclusive gap guard");
    require(locks.lock(b,LockMode::S,KeyRangeEnt::open(10,20),LockKind::Gap)==RCOK,"shared gap coexist");clear();
    require(locks.lock(a,LockMode::X,KeyRangeEnt::point(15),LockKind::InsertIntent)==RCOK,"position one");
    require(locks.lock(b,LockMode::X,KeyRangeEnt::point(16),LockKind::InsertIntent)==RCOK,"position two");clear();
    require(locks.get_lock_count()==0,"classic contract leaves no locks");
}
}
