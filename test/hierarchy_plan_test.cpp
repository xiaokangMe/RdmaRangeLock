#include "hierarchy_contract.h"
#include "lock/lock_manager.h"
int main() {
    hierarchy_test::plan_contract();
    rrl::LockManager manager;
    manager.set_bucket_width(256);
    hierarchy_test::contract(manager);
    std::cout << "hierarchy record granularity and range protection: OK\n";
}
