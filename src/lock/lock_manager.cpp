#include "lock/lock_manager.h"

#include <algorithm>

namespace rrl {

RC LockManager::lock(txnid_t txn_id, LockMode mode, const KeyRangeEnt& range,
                     LockKind kind, std::vector<txnid_t>* conflicts) {
    if (range.empty() || !range.valid()) return ERROR;

    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<txnid_t> found;
    RangeLock request{txn_id, mode, kind, range};
    for (const RangeLock& held : locks_) {
        if (!request.conflicts(held)) continue;
        if (std::find(found.begin(), found.end(), held.txn_id) == found.end()) found.push_back(held.txn_id);
    }
    if (conflicts != nullptr) *conflicts = found;
    if (!found.empty()) return Abort;

    // Repeated locks are idempotent. A stronger request upgrades the exact
    // interval in place, which preserves strict two-phase locking.
    for (RangeLock& held : locks_) {
        if (held.txn_id == txn_id && held.kind == kind &&
            held.range.StartKey == range.StartKey && held.range.EndKey == range.EndKey &&
            held.range.has_start == range.has_start && held.range.has_end == range.has_end &&
            held.range.start_bound == range.start_bound && held.range.end_bound == range.end_bound) {
            held.mode = lock_mode_join(held.mode, mode);
            return RCOK;
        }
    }
    locks_.push_back(request);
    return RCOK;
}

RC LockManager::unlock(txnid_t txn_id, const KeyRangeEnt& range, LockKind kind) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto old_size = locks_.size();
    locks_.erase(std::remove_if(locks_.begin(), locks_.end(), [&](const RangeLock& held) {
        return held.txn_id == txn_id && held.kind == kind &&
               held.range.StartKey == range.StartKey &&
               held.range.EndKey == range.EndKey && held.range.has_start == range.has_start &&
               held.range.has_end == range.has_end && held.range.start_bound == range.start_bound &&
               held.range.end_bound == range.end_bound;
    }), locks_.end());
    return locks_.size() == old_size ? Abort : RCOK;
}

RC LockManager::unlock_all(txnid_t txn_id) {
    std::lock_guard<std::mutex> guard(mutex_);
    locks_.erase(std::remove_if(locks_.begin(), locks_.end(),
                                [&](const RangeLock& held) { return held.txn_id == txn_id; }),
                 locks_.end());
    return RCOK;
}

std::size_t LockManager::get_lock_count(txnid_t txn_id) const {
    std::lock_guard<std::mutex> guard(mutex_);
    if (txn_id == 0) return locks_.size();
    return static_cast<std::size_t>(std::count_if(locks_.begin(), locks_.end(),
                                                  [&](const RangeLock& held) { return held.txn_id == txn_id; }));
}

bool LockManager::detect_conflict(const RangeLock& request) const {
    std::lock_guard<std::mutex> guard(mutex_);
    return std::any_of(locks_.begin(), locks_.end(), [&](const RangeLock& held) { return request.conflicts(held); });
}

std::vector<txnid_t> LockManager::conflicting_txns(const RangeLock& request) const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<txnid_t> result;
    for (const RangeLock& held : locks_) {
        if (request.conflicts(held) && std::find(result.begin(), result.end(), held.txn_id) == result.end())
            result.push_back(held.txn_id);
    }
    return result;
}

std::vector<RangeLock> LockManager::snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return locks_;
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
    const KeyRangeEnt range = deneva_range(start, end);
    std::vector<txnid_t> conflicts;
    if (manager_.lock(txn, LockMode::X, range, LockKind::Predicate, &conflicts) == RCOK) conflicts.clear();
    return conflicts;
}

RC KeyXidCache::removeReadXidWithMutex(idx_key_t start, idx_key_t end, txnid_t txn, std::int32_t isolation) {
    (void)isolation; return manager_.unlock(txn, deneva_range(start, end), LockKind::Predicate);
}
RC KeyXidCache::removeWriteXidWithMutex(idx_key_t start, idx_key_t end, txnid_t txn, std::int32_t isolation) {
    (void)isolation; return manager_.unlock(txn, deneva_range(start, end), LockKind::Predicate);
}
std::vector<txnid_t> KeyXidCache::getReadXid(idx_key_t start, idx_key_t end, bool is_delete) const {
    (void)is_delete; return manager_.conflicting_txns({0, LockMode::S, LockKind::Predicate, deneva_range(start, end)});
}
std::vector<txnid_t> KeyXidCache::getWriteXid(idx_key_t start, idx_key_t end, bool is_delete) const {
    (void)is_delete; return manager_.conflicting_txns({0, LockMode::X, LockKind::Predicate, deneva_range(start, end)});
}

}  // namespace rrl
