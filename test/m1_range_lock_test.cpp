#include "lock/lock_manager.h"

#include <cassert>
#include <iostream>

using namespace rrl;

int main() {
    const KeyRangeEnt left = KeyRangeEnt::half_open(10, 20);
    const KeyRangeEnt touching = KeyRangeEnt::half_open(20, 30);
    const KeyRangeEnt closed_touching = KeyRangeEnt::closed(20, 30);
    (void)left; (void)touching; (void)closed_touching;
    assert(!left.overlaps(touching));
    assert(KeyRangeEnt::closed(10, 20).overlaps(closed_touching));
    assert(KeyRangeEnt::point(7).contains(7));
    assert(!KeyRangeEnt::point(7).contains(8));
    assert(KeyRangeEnt::from_left(20).contains(100));
    assert(KeyRangeEnt::to_right(20).contains(0));

    LockManager manager;
    assert(manager.lock(1, LockMode::S, left, LockKind::NextKey) == RCOK);
    assert(manager.lock(2, LockMode::S, left, LockKind::NextKey) == RCOK);
    assert(manager.lock(3, LockMode::X, left, LockKind::NextKey) == Abort);
    assert(manager.get_lock_count() == 2);
    assert(manager.unlock_all(1) == RCOK);
    assert(manager.lock(3, LockMode::X, left, LockKind::NextKey) == Abort);
    assert(manager.unlock_all(2) == RCOK);
    assert(manager.lock(3, LockMode::X, left, LockKind::NextKey) == RCOK);
    assert(manager.lock(3, LockMode::X, left, LockKind::NextKey) == RCOK);
    assert(manager.get_lock_count(3) == 1);

    const KeyRangeEnt exact = KeyRangeEnt::closed(40, 50);
    (void)exact;
    assert(manager.lock(4, LockMode::S, exact, LockKind::Predicate) == RCOK);
    assert(manager.lock(4, LockMode::X, exact, LockKind::Predicate) == RCOK);
    assert(manager.snapshot().back().mode == LockMode::X);

    KeyXidCache cache;
    cache.init();
    assert(cache.addReadXidWithMutex(100, 120, 10, 0) == RCOK);
    const auto conflicts = cache.addWriteXidWithMutex(110, 130, 11, 0, false);
    assert(conflicts.size() == 1 && conflicts.front() == 10);
    assert(cache.removeReadXidWithMutex(100, 120, 10, 0) == RCOK);
    assert(cache.addWriteXidWithMutex(110, 130, 11, 0, false).empty());

    std::cout << "M1 range and lock interfaces: OK\n";
    return 0;
}