#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "common/range.h"
#include "common/types.h"

namespace rrl {

// Lightweight, no-wait range lock table. It follows deneva's KeyXidCache
// vocabulary but stores typed intervals instead of sentinel-only endpoints.
class LockManager {
public:
    RC lock(txnid_t txn_id, LockMode mode, const KeyRangeEnt& range,
            LockKind kind = LockKind::Predicate,
            std::vector<txnid_t>* conflicts = nullptr);

    RC unlock(txnid_t txn_id, const KeyRangeEnt& range);
    RC unlock_all(txnid_t txn_id);

    std::size_t get_lock_count(txnid_t txn_id = 0) const;
    bool detect_conflict(const RangeLock& request) const;
    std::vector<txnid_t> conflicting_txns(const RangeLock& request) const;
    std::vector<RangeLock> snapshot() const;

private:
    mutable std::mutex mutex_;
    std::vector<RangeLock> locks_;
};

// Compatibility facade for deneva code that uses KeyXidCache's method names.
class KeyXidCache {
public:
    void init() {}

    RC addReadXidWithMutex(idx_key_t start, idx_key_t end, txnid_t txn,
                           std::int32_t isolation);
    std::vector<txnid_t> addWriteXidWithMutex(idx_key_t start, idx_key_t end,
                                              txnid_t txn,
                                              std::int32_t isolation,
                                              bool is_delete);
    RC removeReadXidWithMutex(idx_key_t start, idx_key_t end, txnid_t txn,
                              std::int32_t isolation);
    RC removeWriteXidWithMutex(idx_key_t start, idx_key_t end, txnid_t txn,
                               std::int32_t isolation);

    std::vector<txnid_t> getReadXid(idx_key_t start, idx_key_t end,
                                    bool is_delete) const;
    std::vector<txnid_t> getWriteXid(idx_key_t start, idx_key_t end,
                                     bool is_delete) const;
    std::size_t getKeyXidSize() const { return manager_.get_lock_count(); }

    LockManager& manager() { return manager_; }
    const LockManager& manager() const { return manager_; }

private:
    static KeyRangeEnt deneva_range(idx_key_t start, idx_key_t end);
    LockManager manager_;
};

}  // namespace rrl