#include "index/btree.h"
#include "lock/lock_manager.h"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

using namespace rrl;

static KeyRangeEnt next_key(idx_key_t left, idx_key_t right) {
    KeyRangeEnt range;
    range.StartKey = left; range.EndKey = right;
    range.has_start = range.has_end = true;
    range.start_bound = Bound::Open; range.end_bound = Bound::Closed;
    return range;
}

int main() {
    BTreeLatch tree;
    tree.init(8);
    std::uint64_t a = 10, b = 20, c = 30;
    tree.insert(10, &a); tree.insert(20, &b); tree.insert(30, &c);
    LockManager locks;
    std::mutex mutex;
    std::condition_variable cv;
    bool range_locked = false;
    RC insert_rc = ERROR;
    std::thread reader([&] {
        if (locks.lock(1, LockMode::S, next_key(10, 20), LockKind::NextKey) != RCOK) return;
        if (locks.lock(1, LockMode::S, next_key(20, 30), LockKind::NextKey) != RCOK) return;
        {
            std::lock_guard<std::mutex> guard(mutex);
            range_locked = true;
        }
        cv.notify_one();
        std::unique_lock<std::mutex> guard(mutex);
        cv.wait(guard, [&] { return insert_rc != ERROR; });
        locks.unlock_all(1);
    });
    std::thread writer([&] {
        std::unique_lock<std::mutex> guard(mutex);
        cv.wait(guard, [&] { return range_locked; });
        guard.unlock();
        insert_rc = locks.lock(2, LockMode::X, KeyRangeEnt::point(15), LockKind::NextKey);
        cv.notify_one();
    });
    reader.join(); writer.join();
    if (insert_rc != Abort) return 1;
    if (tree.scan_range(KeyRangeEnt::closed(10, 30)).size() != 3) return 2;

    if (locks.lock(3, LockMode::S, next_key(40, 50), LockKind::NextKey) != RCOK) return 3;
    if (locks.lock(4, LockMode::X, KeyRangeEnt::point(45), LockKind::NextKey) != Abort) return 4;
    locks.unlock_all(3);
    if (locks.lock(4, LockMode::X, KeyRangeEnt::point(45), LockKind::NextKey) != RCOK) return 5;

    if (locks.lock(5, LockMode::S, KeyRangeEnt::point(60), LockKind::Record) != RCOK) return 6;
    if (locks.lock(5, LockMode::S, KeyRangeEnt::point(60), LockKind::Gap) != RCOK) return 7;
    if (locks.unlock(5, KeyRangeEnt::point(60), LockKind::Record) != RCOK) return 8;
    if (locks.get_lock_count(5) != 1) return 9;
    locks.unlock_all(4); locks.unlock_all(5);
    std::cout << "v5 next-key lock phantom prevention: OK\n";
    return 0;
}
