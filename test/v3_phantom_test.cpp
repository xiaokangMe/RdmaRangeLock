#include "index/btree.h"
#include "lock/lock_manager.h"
#include <iostream>
using namespace rrl;
int main(){BTreeLatch tree;tree.init(8);uint64_t v10=10,v20=20,v30=30,v15=15;tree.insert(10,&v10);tree.insert(20,&v20);tree.insert(30,&v30);LockManager locks;auto before=tree.scan_range(KeyRangeEnt::closed(10,30));if(before.size()!=3)return 1;for(auto& row:before)if(locks.lock(1,LockMode::S,KeyRangeEnt::point(row.key),LockKind::Record)!=RCOK)return 1;if(locks.lock(2,LockMode::X,KeyRangeEnt::point(15),LockKind::Record)!=RCOK)return 1;tree.insert(15,&v15);locks.unlock_all(2);auto after=tree.scan_range(KeyRangeEnt::closed(10,30));locks.unlock_all(1);if(after.size()!=4)return 1;std::cout<<"v3 record 2PL phantom reproduction: OK\n";}