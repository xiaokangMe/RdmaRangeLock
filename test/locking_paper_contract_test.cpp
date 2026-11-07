#include "locking_paper_contract.h"
#include "classic_range_contract.h"
#include <iostream>
int main() {
    try {
        rrl::LockManager locks;
        paper_contract::check(locks);
    classic_contract::check(locks);
        std::cout<<"Eswaran single-interval / Gray four-mode histories: OK\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
