#include "index/btree.h"
#include "lock/lock_manager.h"
#include <iostream>
#include <stdexcept>
using namespace rrl;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try {
    BTreeLatch tree;tree.init(8);uint64_t a=10,b=20,c=30,d=15;
    tree.insert(10,&a);tree.insert(20,&b);tree.insert(30,&c);LockManager locks;
    check(locks.lock(1,LockMode::S,KeyRangeEnt::open(10,30),LockKind::Gap)==RCOK,"gap S");
    check(locks.lock(2,LockMode::X,KeyRangeEnt::open(10,30),LockKind::Gap)==RCOK,"gap X coexists");
    check(locks.lock(3,LockMode::X,KeyRangeEnt::point(20),LockKind::Record)==RCOK,"guard does not block existing record");
    check(locks.lock(3,LockMode::X,KeyRangeEnt::point(15),LockKind::InsertIntent)==AbortConflict,"guard blocks insertion");
    check(locks.unlock_all(1)==RCOK && locks.unlock_all(2)==RCOK && locks.unlock_all(3)==RCOK,"release guards");
    check(locks.lock(2,LockMode::X,KeyRangeEnt::point(15),LockKind::InsertIntent)==RCOK,"first insert position");
    check(locks.lock(3,LockMode::X,KeyRangeEnt::point(16),LockKind::InsertIntent)==RCOK,"different insert position");
    check(locks.lock(3,LockMode::X,KeyRangeEnt::point(15),LockKind::InsertIntent)==AbortConflict,"same position excludes");
    check(locks.lock(1,LockMode::S,KeyRangeEnt::open(10,30),LockKind::Gap)==AbortConflict,"writer-first guard excludes");
    check(tree.insert(15,&d)==RCOK,"insert");
    check(locks.unlock_all(2)==RCOK && locks.unlock_all(3)==RCOK,"insert commit");
    check(tree.scan_range(KeyRangeEnt::closed(10,30)).size()==4 && locks.get_lock_count()==0,"no phantom before release or leaked lock");
    std::cout<<"Inhibitive gap guards, insert intentions and record independence: OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
