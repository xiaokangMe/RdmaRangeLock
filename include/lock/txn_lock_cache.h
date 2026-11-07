#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/range.h"

namespace rrl {

// Per-transaction bookkeeping for the remote lock manager: which shards a
// transaction touched, and which locks it already holds.
//
// Three properties matter here and are why this is a class of its own:
//   * entries are split across independent shards, so a manager-wide mutex is
//     not on the path of every lock request;
//   * a release must be able to *shrink* a transaction's shard set, otherwise
//     every later unlock keeps visiting shards that hold nothing of its own;
//   * all of it must be testable without an RDMA device.
class TxnLockCache {
public:
    struct CachedLock {
        LockMode mode{LockMode::IS};
        LockKind kind{LockKind::Predicate};
        KeyRangeEnt range{};
    };
    using ShardMapper = std::function<std::vector<std::size_t>(const CachedLock&)>;

    static constexpr std::size_t kShards = 64;

    void remember_shards(txnid_t txn_id, const std::vector<std::size_t>& ids) {
        auto& cache = shard_for(txn_id);
        std::lock_guard<std::mutex> guard(cache.mutex);
        auto& saved = cache.shards[txn_id];
        for (const std::size_t id : ids)
            if (std::find(saved.begin(), saved.end(), id) == saved.end()) saved.push_back(id);
        std::sort(saved.begin(), saved.end());
    }

    std::vector<std::size_t> shards(txnid_t txn_id) const {
        auto& cache = shard_for(txn_id);
        std::lock_guard<std::mutex> guard(cache.mutex);
        const auto it = cache.shards.find(txn_id);
        return it == cache.shards.end() ? std::vector<std::size_t>{} : it->second;
    }

    // Falls back to every shard when the transaction has no recorded entry, so
    // a release still covers a transaction whose bookkeeping was lost.
    std::vector<std::size_t> shards_or_all(txnid_t txn_id, std::size_t count) const {
        auto ids = shards(txn_id);
        if (!ids.empty()) return ids;
        std::vector<std::size_t> all(count);
        for (std::size_t i = 0; i < count; ++i) all[i] = i;
        return all;
    }

    bool satisfies(txnid_t txn_id, LockMode mode, const KeyRangeEnt& range,
                   LockKind kind) const {
        auto& cache = shard_for(txn_id);
        std::lock_guard<std::mutex> guard(cache.mutex);
        const auto it = cache.locks.find(txn_id);
        if (it == cache.locks.end()) return false;
        for (const auto& held : it->second) {
            if (held.kind == kind && same_range(held.range, range) &&
                lock_mode_join(held.mode, mode) == held.mode) return true;
        }
        return false;
    }

    void remember(txnid_t txn_id, LockMode mode, const KeyRangeEnt& range, LockKind kind) {
        auto& cache = shard_for(txn_id);
        std::lock_guard<std::mutex> guard(cache.mutex);
        auto& saved = cache.locks[txn_id];
        for (auto& held : saved) {
            if (held.kind == kind && same_range(held.range, range)) {
                held.mode = lock_mode_join(held.mode, mode);
                return;
            }
        }
        saved.push_back(CachedLock{mode, kind, range});
    }

    bool replace_mode(txnid_t txn_id,LockMode mode,const KeyRangeEnt& range,LockKind kind) {
        auto& cache=shard_for(txn_id);std::lock_guard<std::mutex> guard(cache.mutex);
        const auto it=cache.locks.find(txn_id);if(it==cache.locks.end())return false;
        for(auto& held:it->second)if(held.kind==kind && same_range(held.range,range)){held.mode=mode;return true;}
        return false;
    }

    void forget(txnid_t txn_id, const KeyRangeEnt& range, LockKind kind) {
        auto& cache = shard_for(txn_id);
        std::lock_guard<std::mutex> guard(cache.mutex);
        const auto it = cache.locks.find(txn_id);
        if (it == cache.locks.end()) return;
        auto& saved = it->second;
        saved.erase(std::remove_if(saved.begin(), saved.end(), [&](const CachedLock& held) {
            return held.kind == kind && same_range(held.range, range);
        }), saved.end());
        if (saved.empty()) cache.locks.erase(it);
    }

    void forget_shard(txnid_t txn_id, std::size_t shard_id) {
        auto& cache = shard_for(txn_id);
        std::lock_guard<std::mutex> guard(cache.mutex);
        const auto it = cache.shards.find(txn_id);
        if (it == cache.shards.end()) return;
        auto& saved = it->second;
        saved.erase(std::remove(saved.begin(), saved.end(), shard_id), saved.end());
        if (saved.empty()) cache.shards.erase(it);
    }

    void drop(txnid_t txn_id) {
        auto& cache = shard_for(txn_id);
        std::lock_guard<std::mutex> guard(cache.mutex);
        cache.shards.erase(txn_id);
        cache.locks.erase(txn_id);
    }

    std::size_t lock_count(txnid_t txn_id) const {
        auto& cache = shard_for(txn_id);
        std::lock_guard<std::mutex> guard(cache.mutex);
        const auto it = cache.locks.find(txn_id);
        return it == cache.locks.end() ? 0 : it->second.size();
    }

    // Recomputes the shard set from the locks still held and drops the entry
    // entirely once the transaction holds nothing. Called after a single
    // unlock, so a shard that no longer holds anything stops being visited.
    void refresh_shards(txnid_t txn_id, const ShardMapper& mapper) {
        auto& cache = shard_for(txn_id);
        std::lock_guard<std::mutex> guard(cache.mutex);
        const auto it = cache.locks.find(txn_id);
        if (it == cache.locks.end() || it->second.empty()) {
            cache.locks.erase(txn_id);
            cache.shards.erase(txn_id);
            return;
        }
        std::vector<std::size_t> ids;
        for (const auto& held : it->second) {
            for (const std::size_t id : mapper(held))
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end());
        if (ids.empty()) cache.shards.erase(txn_id);
        else cache.shards[txn_id] = std::move(ids);
    }

    void clear() {
        for (auto& cache : shards_) {
            std::lock_guard<std::mutex> guard(cache.mutex);
            cache.shards.clear();
            cache.locks.clear();
        }
    }

private:
    static bool same_range(const KeyRangeEnt& lhs, const KeyRangeEnt& rhs) {
        return lhs.StartKey == rhs.StartKey && lhs.EndKey == rhs.EndKey &&
               lhs.has_start == rhs.has_start && lhs.has_end == rhs.has_end &&
               lhs.start_bound == rhs.start_bound && lhs.end_bound == rhs.end_bound;
    }

    struct CacheShard {
        mutable std::mutex mutex;
        std::unordered_map<txnid_t, std::vector<std::size_t>> shards;
        std::unordered_map<txnid_t, std::vector<CachedLock>> locks;
    };

    CacheShard& shard_for(txnid_t txn_id) const {
        return shards_[static_cast<std::size_t>(txn_id % kShards)];
    }

    mutable std::array<CacheShard, kShards> shards_;
};

}  // namespace rrl
