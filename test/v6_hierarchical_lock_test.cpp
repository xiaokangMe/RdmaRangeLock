#include "lock/lock_manager.h"
#include <iostream>

using namespace rrl;

static KeyRangeEnt bucket(idx_key_t lo, idx_key_t hi) { return KeyRangeEnt::closed(lo, hi); }

int main() {
    LockManager locks;
    const auto root = KeyRangeEnt::unbounded();
    if (locks.lock(1, LockMode::IS, root, LockKind::Node) != RCOK) return 1;
    if (locks.lock(2, LockMode::IX, root, LockKind::Node) != RCOK) return 2;
    if (locks.lock(1, LockMode::S, bucket(0, 63), LockKind::Node) != RCOK) return 3;
    if (locks.lock(2, LockMode::X, bucket(64, 127), LockKind::Node) != RCOK) return 4;
    if (locks.lock(3, LockMode::X, bucket(0, 63), LockKind::Node) != Abort) return 5;
    if (locks.lock(3, LockMode::X, bucket(128, 191), LockKind::Node) != RCOK) return 6;
    locks.unlock_all(1); locks.unlock_all(2); locks.unlock_all(3);
    if (locks.lock(4, LockMode::IS, root, LockKind::Node) != RCOK) return 7;
    if (locks.lock(4, LockMode::IX, root, LockKind::Node) != RCOK) return 8;
    const auto snapshot = locks.snapshot();
    if (snapshot.size() != 1 || snapshot.front().mode != LockMode::IX) return 9;
    std::cout << "v6 hierarchical intention locking: OK\n";
    return 0;
}