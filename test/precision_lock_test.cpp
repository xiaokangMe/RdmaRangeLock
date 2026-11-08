#include "precision_contract.h"
#include "../src/txn/precision_plan.h"
#include <atomic>
#include <iostream>
#include <thread>

using namespace rrl;
int main() {try {
    LockManager locks;precision_contract::check(locks);
    for(auto kind:{OperationKind::Read,OperationKind::Update,OperationKind::Insert,OperationKind::Delete}) {
        std::vector<RangeLock> plan;
        Operation op{kind,42,0,0};
        const auto add=[&](LockMode m,const KeyRangeEnt& r,LockKind k){plan.push_back({1,m,k,r});return true;};
        precision_contract::require(detail::precision_point(op,add),"plan success");
        precision_contract::require(plan.size()==(kind==OperationKind::Read?1:2),"implicit read in all writes");
        precision_contract::require(plan[0].kind==LockKind::PrecisionPredicate,"read before update");
        if(plan.size()==2)precision_contract::require(plan[1].kind==LockKind::PrecisionUpdate,"actual tuple Lu");
    }
    // Force simultaneous requests, keep both grants alive until both reply.
    // Atomic check+publication must never let a conflicting Lp/Lu pair pass.
    for(unsigned round=0;round<200;++round) {
        std::atomic<unsigned> ready{0},done{0};std::atomic<bool> go{false};
        RC r[2]={ERROR,ERROR};
        std::thread a([&]{++ready;while(!go.load())std::this_thread::yield();
            r[0]=locks.lock(101,LockMode::S,KeyRangeEnt::closed(0,1000),LockKind::PrecisionPredicate);
            ++done;while(done.load()!=2)std::this_thread::yield();});
        std::thread b([&]{++ready;while(!go.load())std::this_thread::yield();
            r[1]=locks.lock(102,LockMode::X,KeyRangeEnt::point(63),LockKind::PrecisionUpdate);
            ++done;while(done.load()!=2)std::this_thread::yield();});
        while(ready.load()!=2)std::this_thread::yield();
        go=true;a.join();b.join();
        precision_contract::require((r[0]==RCOK && r[1]==AbortConflict) ||
                                   (r[1]==RCOK && r[0]==AbortConflict),"atomic Lp/Lu publication race");
        locks.unlock_all(101);locks.unlock_all(102);
    }
    precision_contract::require(locks.get_lock_count()==0,"no leaked metadata");
    std::cout<<"Jordan1981 Lp/Lu, implicit read, Griffin membership/value separation and publication races: OK\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
