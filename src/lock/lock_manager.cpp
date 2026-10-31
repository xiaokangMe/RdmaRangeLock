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
        (mode == LockMode::IS || mode == LockMode::IX)) return {0};
    return shard_ids(range);
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

RC LockManager::lock(txnid_t txn_id, LockMode mode, const KeyRangeEnt& range,
                     LockKind kind, std::vector<txnid_t>* conflicts) {
    if (range.empty() || !range.valid()) return ERROR;
    if (conflicts) conflicts->clear();
    const auto ids = shard_ids(range, kind, mode);
    std::vector<std::unique_lock<std::mutex>> guards;
    guards.reserve(ids.size());
    for (const auto id : ids) guards.emplace_back(shards_[id].mutex);

    RangeLock request{txn_id, mode, kind, range};
    std::vector<txnid_t> found;
    for (const auto id : ids) {
        for (const RangeLock& held : shards_[id].locks) {
            if (!request.conflicts(held)) continue;
            if (std::find(found.begin(), found.end(), held.txn_id) == found.end())
                found.push_back(held.txn_id);
        }
    }
    if (conflicts) *conflicts = found;
    if (!found.empty()) return AbortConflict;

    for (const auto id : ids) {
        auto& table = shards_[id].locks;
        auto it = std::find_if(table.begin(), table.end(), [&](const RangeLock& held) {
            return held.txn_id == txn_id && held.kind == kind && same_range(held.range, range);
        });
        if (it != table.end()) it->mode = lock_mode_join(it->mode, mode);
        else table.push_back(request);
    }
    return RCOK;
}

RC LockManager::unlock(txnid_t txn_id, const KeyRangeEnt& range, LockKind kind) {
    const auto ids = (kind == LockKind::Node && range.is_unbounded())
        ? std::vector<std::size_t>{0} : shard_ids(range);
    std::vector<std::unique_lock<std::mutex>> guards;
    guards.reserve(ids.size());
    for (const auto id : ids) guards.emplace_back(shards_[id].mutex);
    bool removed = false;
    for (const auto id : ids) {
        auto& table = shards_[id].locks;
        const auto before = table.size();
        table.erase(std::remove_if(table.begin(), table.end(), [&](const RangeLock& held) {
            return held.txn_id == txn_id && held.kind == kind && same_range(held.range, range);
        }), table.end());
        removed = removed || table.size() != before;
    }
    return removed ? RCOK : Abort;
}

RC LockManager::unlock_all(txnid_t txn_id) {
    const auto ids = all_shards();
    std::vector<std::unique_lock<std::mutex>> guards;
    guards.reserve(ids.size());
    for (const auto id : ids) guards.emplace_back(shards_[id].mutex);
    for (const auto id : ids) {
        auto& table = shards_[id].locks;
        table.erase(std::remove_if(table.begin(), table.end(), [&](const RangeLock& held) {
            return held.txn_id == txn_id;
        }), table.end());
    }
    return RCOK;
}

std::size_t LockManager::get_lock_count(txnid_t txn_id) const {
    const auto ids = all_shards();
    std::vector<std::unique_lock<std::mutex>> guards;
    guards.reserve(ids.size());
    for (const auto id : ids) guards.emplace_back(shards_[id].mutex);
    std::vector<RangeLock> unique;
    for (const auto id : ids) for (const auto& lock : shards_[id].locks) {
        if (txn_id != 0 && lock.txn_id != txn_id) continue;
        if (std::find_if(unique.begin(), unique.end(), [&](const RangeLock& other) {
                return same_lock(lock, other);
            }) == unique.end()) unique.push_back(lock);
    }
    return unique.size();
}

bool LockManager::detect_conflict(const RangeLock& request) const {
    const auto ids = shard_ids(request.range, request.kind, request.mode);
    std::vector<std::unique_lock<std::mutex>> guards;
    guards.reserve(ids.size());
    for (const auto id : ids) guards.emplace_back(shards_[id].mutex);
    for (const auto id : ids)
        for (const auto& held : shards_[id].locks)
            if (request.conflicts(held)) return true;
    return false;
}

std::vector<txnid_t> LockManager::conflicting_txns(const RangeLock& request) const {
    const auto ids = shard_ids(request.range, request.kind, request.mode);
    std::vector<std::unique_lock<std::mutex>> guards;
    guards.reserve(ids.size());
    for (const auto id : ids) guards.emplace_back(shards_[id].mutex);
    std::vector<txnid_t> result;
    for (const auto id : ids) for (const auto& held : shards_[id].locks) {
        if (request.conflicts(held) &&
            std::find(result.begin(), result.end(), held.txn_id) == result.end())
            result.push_back(held.txn_id);
    }
    return result;
}

std::vector<RangeLock> LockManager::snapshot() const {
    const auto ids = all_shards();
    std::vector<std::unique_lock<std::mutex>> guards;
    guards.reserve(ids.size());
    for (const auto id : ids) guards.emplace_back(shards_[id].mutex);
    std::vector<RangeLock> result;
    for (const auto id : ids) for (const auto& lock : shards_[id].locks)
        if (std::find_if(result.begin(), result.end(), [&](const RangeLock& other) {
                return same_lock(lock, other);
            }) == result.end()) result.push_back(lock);
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
