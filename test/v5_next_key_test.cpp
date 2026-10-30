#include "lock/lock_manager.h"
#include <iostream>

using namespace rrl;

static KeyRangeEnt next_key(idx_key_t left, idx_key_t right) {
    KeyRangeEnt range;
    range.StartKey = left; range.EndKey = right;
    range.has_start = range.has_end = true;
    range.start_bound = Bound::Open; range.end_bound = Bound::Closed;
    return range;
}

int main() {
    LockManager locks;
    if (locks.lock(1, LockMode::S, next_key(10, 20), LockKind::NextKey) != RCOK) return 1;
    if (locks.lock(1, LockMode::S, next_key(20, 30), LockKind::NextKey) != RCOK) return 2;
    if (locks.lock(2, LockMode::X, KeyRangeEnt::point(15), LockKind::NextKey) != Abort) return 3;
    if (locks.lock(2, LockMode::X, KeyRangeEnt::point(25), LockKind::NextKey) != Abort) return 4;
    if (locks.lock(2, LockMode::X, KeyRangeEnt::point(35), LockKind::NextKey) != RCOK) return 5;
    locks.unlock_all(2);
    locks.unlock_all(1);
    if (locks.lock(3, LockMode::S, next_key(40, 50), LockKind::NextKey) != RCOK) return 6;
    if (locks.lock(4, LockMode::X, KeyRangeEnt::point(45), LockKind::NextKey) != Abort) return 7;
    locks.unlock_all(3);
    if (locks.lock(4, LockMode::X, KeyRangeEnt::point(45), LockKind::NextKey) != RCOK) return 8;
    std::cout << "v5 next-key lock phantom prevention: OK\n";
    return 0;
}