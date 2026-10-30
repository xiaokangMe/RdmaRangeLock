#include "index/btree.h"
#include "lock/lock_manager.h"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

using namespace rrl;

static KeyRangeEnt bucket(idx_key_t lo, idx_key_t hi) { return KeyRangeEnt::closed(lo, hi); }

int main() {
    BTreeLatch tree;
    tree.init(8);
    std::uint64_t a = 10, b = 20;
    tree.insert(10, &a); tree.insert(20, &b);
    LockManager locks;
    const auto root = KeyRangeEnt::unbounded();
    std::mutex mutex;
    std::condition_variable cv;
    bool reader_ready = false;
    RC writer_rc = ERROR;
    std::thread reader([&] {
        if (locks.lock(1, LockMode::IS, root, LockKind::Node) != RCOK) return;
        if (locks.lock(1, LockMode::S, bucket(0, 63), LockKind::Node) != RCOK) return;
        {
            std::lock_guard<std::mutex> guard(mutex);
            reader_ready = true;
        }
        cv.notify_one();
        std::unique_lock<std::mutex> guard(mutex);
        cv.wait(guard, [&] { return writer_rc != ERROR; });
        locks.unlock_all(1);
    });
    std::thread writer([&] {
        std::unique_lock<std::mutex> guard(mutex);
        cv.wait(guard, [&] { return reader_ready; });
        guard.unlock();
        RC rc = Abort;
        if (locks.lock(2, LockMode::IX, root, LockKind::Node) == RCOK)
            rc = locks.lock(2, LockMode::X, bucket(0, 63), LockKind::Node);
        {
            std::lock_guard<std::mutex> update(mutex);
            writer_rc = rc;
        }
        cv.notify_one();
    });
    reader.join(); writer.join();
    if (writer_rc != Abort) return 1;
    if (tree.scan_range(KeyRangeEnt::closed(10, 20)).size() != 2) return 2;

    if (locks.lock(3, LockMode::IS, root, LockKind::Node) != RCOK) return 3;
    if (locks.lock(3, LockMode::S, bucket(0, 63), LockKind::Node) != RCOK) return 4;
    if (locks.lock(4, LockMode::IX, root, LockKind::Node) != RCOK) return 5;
    if (locks.lock(4, LockMode::X, bucket(64, 127), LockKind::Node) != RCOK) return 6;
    if (locks.lock(5, LockMode::X, bucket(0, 63), LockKind::Node) != Abort) return 7;
    locks.unlock_all(3); locks.unlock_all(4);
    if (locks.lock(5, LockMode::X, bucket(0, 63), LockKind::Node) != RCOK) return 8;
    locks.unlock_all(5);
    std::cout << "v6 hierarchical intention locking: OK\n";
    return 0;
}
