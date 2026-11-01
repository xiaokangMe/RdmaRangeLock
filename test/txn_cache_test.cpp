#include "lock/txn_lock_cache.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace rrl;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

std::vector<std::size_t> shard_of(const TxnLockCache::CachedLock& lock) {
    return {static_cast<std::size_t>(lock.range.StartKey % 16)};
}

}  // namespace

int main() {
    TxnLockCache cache;

    // Two locks in two different shards.
    cache.remember(1, LockMode::S, KeyRangeEnt::point(3), LockKind::NextKey);
    cache.remember_shards(1, {3, 3, 3});
    cache.remember(1, LockMode::S, KeyRangeEnt::point(20), LockKind::NextKey);
    cache.remember_shards(1, {4, 3});

    check(cache.lock_count(1) == 2, "two locks recorded");
    check(cache.shards(1) == std::vector<std::size_t>({3, 4}), "shards deduplicated and sorted");

    // Mode upgrade: a shared lock later requested exclusively must satisfy both.
    check(cache.satisfies(1, LockMode::S, KeyRangeEnt::point(3), LockKind::NextKey),
          "shared lock satisfies a shared request");
    check(!cache.satisfies(1, LockMode::X, KeyRangeEnt::point(3), LockKind::NextKey),
          "shared lock does not satisfy an exclusive request");
    cache.remember(1, LockMode::X, KeyRangeEnt::point(3), LockKind::NextKey);
    check(cache.satisfies(1, LockMode::X, KeyRangeEnt::point(3), LockKind::NextKey),
          "upgraded lock satisfies an exclusive request");
    check(cache.lock_count(1) == 2, "upgrade must not add a second entry");

    // A lock in another kind or range is a different entry.
    check(!cache.satisfies(1, LockMode::S, KeyRangeEnt::point(3), LockKind::Record),
          "kind is part of the identity");
    check(!cache.satisfies(1, LockMode::S, KeyRangeEnt::point(4), LockKind::NextKey),
          "range is part of the identity");

    // Releasing one lock must shrink the shard set, otherwise every later
    // release keeps visiting a shard that holds nothing of this transaction.
    cache.forget(1, KeyRangeEnt::point(3), LockKind::NextKey);
    check(cache.lock_count(1) == 1, "one lock left after forget");
    check(cache.shards(1) == std::vector<std::size_t>({3, 4}), "shards unchanged before refresh");
    cache.refresh_shards(1, shard_of);
    check(cache.shards(1) == std::vector<std::size_t>({4}), "refreshed shard set drops the empty shard");

    // Releasing the last lock must clear the transaction entirely.
    cache.forget(1, KeyRangeEnt::point(20), LockKind::NextKey);
    cache.refresh_shards(1, shard_of);
    check(cache.lock_count(1) == 0, "no locks left");
    check(cache.shards(1).empty(), "no shards left");

    // Transactions must not share entries, including when they hash to the same
    // cache shard (txn 1 and txn 65 share 65 % 64).
    cache.remember(65, LockMode::X, KeyRangeEnt::closed(1, 9), LockKind::Predicate);
    check(cache.lock_count(1) == 0, "neighbouring transaction is independent");
    check(cache.lock_count(65) == 1, "neighbouring transaction has its own entry");
    cache.drop(65);
    check(cache.shards(65).empty() && cache.lock_count(65) == 0, "drop clears shards and locks");

    // A transaction with no bookkeeping falls back to every shard so a release
    // still covers it.
    check(cache.shards(9999).empty(), "unknown transaction has no shards");
    check(cache.shards_or_all(9999, 16).size() == 16, "unknown transaction falls back to all shards");

    // forget_shard only removes the wanted id.
    cache.remember_shards(2, {1, 2, 3});
    cache.forget_shard(2, 2);
    check(cache.shards(2) == std::vector<std::size_t>({1, 3}), "forget_shard removes one id");
    cache.forget_shard(2, 1);
    cache.forget_shard(2, 3);
    check(cache.shards(2).empty(), "forget_shard empties the entry");

    cache.clear();
    check(cache.lock_count(1) == 0 && cache.shards(65).empty(), "clear drops everything");

    if (failures != 0) return 1;
    std::cout << "txn lock cache: OK\n";
    return 0;
}
