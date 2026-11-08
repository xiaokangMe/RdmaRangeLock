#include "lock/lock_manager.h"

#include <algorithm>

namespace rrl {
namespace {

std::vector<std::size_t> all_shards() {
    std::vector<std::size_t> ids(kLocalLockShards);
    for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = i;
    return ids;
}

}  // namespace

bool LockManager::same_range(const KeyRangeEnt& lhs, const KeyRangeEnt& rhs) {
    return lhs.StartKey == rhs.StartKey && lhs.EndKey == rhs.EndKey &&
           lhs.has_start == rhs.has_start && lhs.has_end == rhs.has_end &&
           lhs.start_bound == rhs.start_bound && lhs.end_bound == rhs.end_bound;
}

bool LockManager::same_lock(const RangeLock& lhs, const RangeLock& rhs) {
    return lhs.txn_id == rhs.txn_id && lhs.mode == rhs.mode && lhs.kind == rhs.kind &&
           same_range(lhs.range, rhs.range);
}

void LockManager::set_bucket_width(std::uint64_t width) {
    if (width != 0) bucket_width_ = width;
}

std::vector<std::size_t> LockManager::shard_ids(const KeyRangeEnt& range,
                                                  LockKind kind, LockMode mode) const {
    // Hierarchical intention locks describe the logical root. They need one
    // metadata shard; their compatibility with descendant locks is handled by
    // RangeLock::conflicts, so copying them to every shard only adds mutex
    // traffic.
    if (kind == LockKind::Node && range.is_unbounded() &&
        lock_mode_is_intent(mode)) return {0};
    return shard_ids(range);
}

void LockManager::remember_shards(txnid_t txn_id,
                                   const std::vector<std::size_t>& ids) const {
    std::lock_guard<std::mutex> guard(txn_shards_mutex_);
    auto& saved = txn_shards_[txn_id];
    for (const std::size_t shard : ids)
        if (std::find(saved.begin(), saved.end(), shard) == saved.end()) saved.push_back(shard);
    std::sort(saved.begin(), saved.end());
}

std::vector<std::size_t> LockManager::tracked_shards(txnid_t txn_id) const {
    std::lock_guard<std::mutex> guard(txn_shards_mutex_);
    const auto it = txn_shards_.find(txn_id);
    return it == txn_shards_.end() ? std::vector<std::size_t>{} : it->second;
}

void LockManager::forget_shard(txnid_t txn_id, std::size_t shard) const {
    std::lock_guard<std::mutex> guard(txn_shards_mutex_);
    const auto it = txn_shards_.find(txn_id);
    if (it == txn_shards_.end()) return;
    auto& saved = it->second;
    saved.erase(std::remove(saved.begin(), saved.end(), shard), saved.end());
    if (saved.empty()) txn_shards_.erase(it);
}

void LockManager::forget_txn(txnid_t txn_id) const {
    std::lock_guard<std::mutex> guard(txn_shards_mutex_);
    txn_shards_.erase(txn_id);
}

std::vector<std::size_t> LockManager::shard_ids(const KeyRangeEnt& range) const {
    if (!range.has_start || !range.has_end) return all_shards();
    const std::uint64_t first = range.StartKey / bucket_width_;
    std::uint64_t last = range.EndKey / bucket_width_;
    if (range.end_bound == Bound::Open && range.EndKey != 0 &&
        range.EndKey % bucket_width_ == 0) {
        --last;
    }
    if (last < first || last - first + 1 >= kLocalLockShards) return all_shards();
    std::vector<std::size_t> ids;
    for (std::uint64_t bucket = first;; ++bucket) {
        const auto shard = static_cast<std::size_t>(bucket % kLocalLockShards);
        if (std::find(ids.begin(), ids.end(), shard) == ids.end()) ids.push_back(shard);
        if (bucket == last) break;
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

namespace {

bool is_point_range(const KeyRangeEnt& range) {
    return range.has_start && range.has_end && range.StartKey == range.EndKey &&
           range.start_bound == Bound::Closed && range.end_bound == Bound::Closed;
}

}  // namespace

RC LockManager::lock(txnid_t txn_id, LockMode mode, const KeyRangeEnt& range,
                     LockKind kind, std::vector<txnid_t>* conflicts) {
    if (!txn_id || !valid_lock_mode(mode) || kind>LockKind::GriffinUpdate || range.empty() || !range.valid() || !valid_precision_request(mode,kind,range)) return ERROR;
    if (conflicts) conflicts->clear();
    const auto ids = shard_ids(range, kind, mode);
    std::vector<std::unique_lock<std::mutex>> guards;
    guards.reserve(ids.size());
    for (const auto id : ids) guards.emplace_back(shards_[id].mutex);

    RangeLock request{txn_id, mode, kind, range};
    for (const auto id : ids) {
        const auto& shard = shards_[id];
        auto combine = [&](const RangeLock& held) {
            if (held.txn_id == txn_id && held.kind == kind && same_range(held.range, range))
                request.mode = lock_mode_join(request.mode, held.mode);
        };
        if (is_point_range(range)) {
            const auto it = shard.point_locks.find(range.StartKey);
            if (it != shard.point_locks.end()) for (const auto& held : it->second) combine(held);
        } else for (const auto& held : shard.range_locks) combine(held);
    }
    mode = request.mode;
    std::vector<txnid_t> found;
    auto inspect = [&](const RangeLock& held) {
        if (!request.conflicts(held)) return;
        if (std::find(found.begin(), found.end(), held.txn_id) == found.end())
            found.push_back(held.txn_id);
    };
    for (const auto id : ids) {
        const auto& shard = shards_[id];
        if (is_point_range(range)) {
            const auto it = shard.point_locks.find(range.StartKey);
            if (it != shard.point_locks.end())
                for (const auto& held : it->second) inspect(held);
        } else {
            auto it = range.has_start ? shard.point_locks.lower_bound(range.StartKey)
                                      : shard.point_locks.begin();
            for (; it != shard.point_locks.end(); ++it) {
                if (range.has_end && it->first > range.EndKey) break;
                for (const auto& held : it->second) inspect(held);
            }
        }
        for (const auto& held : shard.range_locks) inspect(held);
    }
    if (conflicts) *conflicts = found;
    if (!found.empty()) return AbortConflict;

    for (const auto id : ids) {
        auto& shard = shards_[id];
        if (is_point_range(range)) {
            auto& table = shard.point_locks[range.StartKey];
            auto it = std::find_if(table.begin(), table.end(), [&](const RangeLock& held) {
                return held.txn_id == txn_id && held.kind == kind && same_range(held.range, range);
            });
            if (it != table.end()) it->mode = lock_mode_join(it->mode, mode);
            else table.push_back(request);
        } else {
            auto& table = shard.range_locks;
            auto it = std::find_if(table.begin(), table.end(), [&](const RangeLock& held) {
                return held.txn_id == txn_id && held.kind == kind && same_range(held.range, range);
            });
            if (it != table.end()) it->mode = lock_mode_join(it->mode, mode);
            else table.push_back(request);
        }
    }
    remember_shards(txn_id, ids);
    return RCOK;
}

RC LockManager::convert_lock(txnid_t txn_id,LockMode mode,const KeyRangeEnt& range,
                                 LockKind kind,std::vector<txnid_t>* conflicts) {
    if(!txn_id || !valid_lock_mode(mode) || kind!=LockKind::Node ||
       !range.has_start || !range.has_end || range.empty() || !range.valid())return ERROR;
    const auto ids=shard_ids(range);
    std::vector<std::unique_lock<std::mutex>> guards;guards.reserve(ids.size());
    for(auto id:ids)guards.emplace_back(shards_[id].mutex);
    if(conflicts)conflicts->clear();
    RangeLock request{txn_id,mode,kind,range};bool owned=false,blocked=false;
    for(auto id:ids) {
        const auto inspect=[&](const RangeLock& held) {
            if(held.txn_id==txn_id && held.kind==kind && same_range(held.range,range))owned=true;
            if(!request.conflicts(held))return;
            blocked=true;
            if(conflicts && std::find(conflicts->begin(),conflicts->end(),held.txn_id)==conflicts->end())conflicts->push_back(held.txn_id);
        };
        for(const auto& entry:shards_[id].point_locks)for(const auto& held:entry.second)inspect(held);
        for(const auto& held:shards_[id].range_locks)inspect(held);
    }
    if(!owned)return ERROR;
    if(blocked)return AbortConflict;
    for(auto id:ids) {
        auto replace=[&](RangeLock& held) {
            if(held.txn_id==txn_id && held.kind==kind && same_range(held.range,range))held.mode=mode;
        };
        for(auto& entry:shards_[id].point_locks)for(auto& held:entry.second)replace(held);
        for(auto& held:shards_[id].range_locks)replace(held);
    }
    return RCOK;
}

RC LockManager::unlock(txnid_t txn_id, const KeyRangeEnt& range, LockKind kind) {
    auto ids = tracked_shards(txn_id);
    if (ids.empty()) ids = shard_ids(range);
    bool removed = false;
    for (const auto id : ids) {
        bool shard_empty_for_txn = true;
        {
            std::lock_guard<std::mutex> guard(shards_[id].mutex);
            auto& shard = shards_[id];
            if (is_point_range(range)) {
                const auto it = shard.point_locks.find(range.StartKey);
                if (it != shard.point_locks.end()) {
                    const auto before = it->second.size();
                    it->second.erase(std::remove_if(it->second.begin(), it->second.end(), [&](const RangeLock& held) {
                        return held.txn_id == txn_id && held.kind == kind && same_range(held.range, range);
                    }), it->second.end());
                    removed = removed || it->second.size() != before;
                    if (it->second.empty()) shard.point_locks.erase(it);
                }
            } else {
                const auto before = shard.range_locks.size();
                shard.range_locks.erase(std::remove_if(shard.range_locks.begin(), shard.range_locks.end(), [&](const RangeLock& held) {
                    return held.txn_id == txn_id && held.kind == kind && same_range(held.range, range);
                }), shard.range_locks.end());
                removed = removed || shard.range_locks.size() != before;
            }
            for (const auto& entry : shard.point_locks)
                for (const auto& held : entry.second)
                    if (held.txn_id == txn_id) shard_empty_for_txn = false;
            if (shard_empty_for_txn)
                for (const auto& held : shard.range_locks)
                    if (held.txn_id == txn_id) shard_empty_for_txn = false;
        }
        if (shard_empty_for_txn) forget_shard(txn_id, id);
    }
    return removed ? RCOK : Abort;
}

RC LockManager::unlock_all(txnid_t txn_id) {
    auto ids = tracked_shards(txn_id);
    if (ids.empty()) ids = all_shards();
    for (const auto id : ids) {
        std::lock_guard<std::mutex> guard(shards_[id].mutex);
        auto& shard = shards_[id];
        for (auto it = shard.point_locks.begin(); it != shard.point_locks.end();) {
            auto& table = it->second;
            table.erase(std::remove_if(table.begin(), table.end(), [&](const RangeLock& held) {
                return held.txn_id == txn_id;
            }), table.end());
            if (table.empty()) it = shard.point_locks.erase(it);
            else ++it;
        }
        shard.range_locks.erase(std::remove_if(shard.range_locks.begin(), shard.range_locks.end(), [&](const RangeLock& held) {
            return held.txn_id == txn_id;
        }), shard.range_locks.end());
    }
    forget_txn(txn_id);
    return RCOK;
}

std::size_t LockManager::get_lock_count(txnid_t txn_id) const {
    auto ids = txn_id == 0 ? all_shards() : tracked_shards(txn_id);
    if (ids.empty()) ids = all_shards();
    std::vector<std::unique_lock<std::mutex>> guards;
    guards.reserve(ids.size());
    for (const auto id : ids) guards.emplace_back(shards_[id].mutex);
    std::vector<RangeLock> unique;
    auto add = [&](const RangeLock& lock) {
        if (txn_id != 0 && lock.txn_id != txn_id) return;
        if (std::find_if(unique.begin(), unique.end(), [&](const RangeLock& other) {
                return same_lock(lock, other);
            }) == unique.end()) unique.push_back(lock);
    };
    for (const auto id : ids) {
        const auto& shard = shards_[id];
        for (const auto& entry : shard.point_locks)
            for (const auto& lock : entry.second) add(lock);
        for (const auto& lock : shard.range_locks) add(lock);
    }
    return unique.size();
}

bool LockManager::detect_conflict(const RangeLock& request) const {
    const auto ids = shard_ids(request.range, request.kind, request.mode);
    std::vector<std::unique_lock<std::mutex>> guards;
    guards.reserve(ids.size());
    for (const auto id : ids) guards.emplace_back(shards_[id].mutex);
    auto inspect = [&](const RangeLock& held) { return request.conflicts(held); };
    for (const auto id : ids) {
        const auto& shard = shards_[id];
        if (is_point_range(request.range)) {
            const auto it = shard.point_locks.find(request.range.StartKey);
            if (it != shard.point_locks.end())
                for (const auto& held : it->second) if (inspect(held)) return true;
        } else {
            auto it = request.range.has_start ? shard.point_locks.lower_bound(request.range.StartKey)
                                              : shard.point_locks.begin();
            for (; it != shard.point_locks.end(); ++it) {
                if (request.range.has_end && it->first > request.range.EndKey) break;
                for (const auto& held : it->second) if (inspect(held)) return true;
            }
        }
        for (const auto& held : shard.range_locks) if (inspect(held)) return true;
    }
    return false;
}

std::vector<txnid_t> LockManager::conflicting_txns(const RangeLock& request) const {
    const auto ids = shard_ids(request.range, request.kind, request.mode);
    std::vector<std::unique_lock<std::mutex>> guards;
    guards.reserve(ids.size());
    for (const auto id : ids) guards.emplace_back(shards_[id].mutex);
    std::vector<txnid_t> result;
    auto inspect = [&](const RangeLock& held) {
        if (request.conflicts(held) &&
            std::find(result.begin(), result.end(), held.txn_id) == result.end())
            result.push_back(held.txn_id);
    };
    for (const auto id : ids) {
        const auto& shard = shards_[id];
        if (is_point_range(request.range)) {
            const auto it = shard.point_locks.find(request.range.StartKey);
            if (it != shard.point_locks.end()) for (const auto& held : it->second) inspect(held);
        } else {
            auto it = request.range.has_start ? shard.point_locks.lower_bound(request.range.StartKey)
                                              : shard.point_locks.begin();
            for (; it != shard.point_locks.end(); ++it) {
                if (request.range.has_end && it->first > request.range.EndKey) break;
                for (const auto& held : it->second) inspect(held);
            }
        }
        for (const auto& held : shard.range_locks) inspect(held);
    }
    return result;
}

std::vector<RangeLock> LockManager::snapshot() const {
    const auto ids = all_shards();
    std::vector<std::unique_lock<std::mutex>> guards;
    guards.reserve(ids.size());
    for (const auto id : ids) guards.emplace_back(shards_[id].mutex);
    std::vector<RangeLock> result;
    auto add = [&](const RangeLock& lock) {
        if (std::find_if(result.begin(), result.end(), [&](const RangeLock& other) {
                return same_lock(lock, other);
            }) == result.end()) result.push_back(lock);
    };
    for (const auto id : ids) {
        const auto& shard = shards_[id];
        for (const auto& entry : shard.point_locks)
            for (const auto& lock : entry.second) add(lock);
        for (const auto& lock : shard.range_locks) add(lock);
    }
    return result;
}

KeyRangeEnt KeyXidCache::deneva_range(idx_key_t start, idx_key_t end) {
    return KeyRangeEnt(start, end);
}

RC KeyXidCache::addReadXidWithMutex(idx_key_t start, idx_key_t end,
                                    txnid_t txn, std::int32_t isolation) {
    (void)isolation;
    return manager_.lock(txn, LockMode::S, deneva_range(start, end), LockKind::Predicate);
}

std::vector<txnid_t> KeyXidCache::addWriteXidWithMutex(
    idx_key_t start, idx_key_t end, txnid_t txn, std::int32_t isolation, bool is_delete) {
    (void)isolation; (void)is_delete;
    std::vector<txnid_t> conflicts;
    const auto range = deneva_range(start, end);
    if (manager_.lock(txn, LockMode::X, range, LockKind::Predicate, &conflicts) == RCOK)
        conflicts.clear();
    return conflicts;
}

RC KeyXidCache::removeReadXidWithMutex(idx_key_t start, idx_key_t end,
                                      txnid_t txn, std::int32_t isolation) {
    (void)isolation;
    return manager_.unlock(txn, deneva_range(start, end), LockKind::Predicate);
}

RC KeyXidCache::removeWriteXidWithMutex(idx_key_t start, idx_key_t end,
                                       txnid_t txn, std::int32_t isolation) {
    (void)isolation;
    return manager_.unlock(txn, deneva_range(start, end), LockKind::Predicate);
}

std::vector<txnid_t> KeyXidCache::getReadXid(idx_key_t start, idx_key_t end, bool is_delete) const {
    (void)is_delete;
    return manager_.conflicting_txns({0, LockMode::S, LockKind::Predicate,
                                      deneva_range(start, end)});
}

std::vector<txnid_t> KeyXidCache::getWriteXid(idx_key_t start, idx_key_t end, bool is_delete) const {
    (void)is_delete;
    return manager_.conflicting_txns({0, LockMode::X, LockKind::Predicate,
                                      deneva_range(start, end)});
}

}  // namespace rrl
