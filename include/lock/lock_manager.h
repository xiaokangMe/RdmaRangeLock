#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "common/range.h"
#include "common/types.h"

namespace rrl {

// Lightweight, no-wait range lock table. Locks are copied into every logical
// key shard touched by an interval, so disjoint intervals do not serialize on
// one process-wide mutex. Shard copies are an index detail; public counts and
// snapshots deduplicate them.
class IRangeLockManager {
public:
    virtual ~IRangeLockManager() = default;
    virtual RC lock(txnid_t txn_id, LockMode mode, const KeyRangeEnt& range,
                    LockKind kind = LockKind::Predicate,
                    std::vector<txnid_t>* conflicts = nullptr) = 0;
    virtual RC unlock(txnid_t txn_id, const KeyRangeEnt& range,
                      LockKind kind = LockKind::Predicate) = 0;
    virtual RC unlock_all(txnid_t txn_id) = 0;
    virtual std::size_t get_lock_count(txnid_t txn_id = 0) const = 0;
};

constexpr std::size_t kLocalLockShards = 16;

class LockManager : public IRangeLockManager {
public:
    RC lock(txnid_t txn_id, LockMode mode, const KeyRangeEnt& range,
            LockKind kind = LockKind::Predicate,
            std::vector<txnid_t>* conflicts = nullptr) override;

    RC unlock(txnid_t txn_id, const KeyRangeEnt& range,
              LockKind kind = LockKind::Predicate) override;
    RC unlock_all(txnid_t txn_id) override;

    std::size_t get_lock_count(txnid_t txn_id = 0) const override;
    bool detect_conflict(const RangeLock& request) const;
    std::vector<txnid_t> conflicting_txns(const RangeLock& request) const;
    std::vector<RangeLock> snapshot() const;
    void set_bucket_width(std::uint64_t width);

private:
    struct Shard {
        mutable std::mutex mutex;
        std::vector<RangeLock> locks;
    };

    std::vector<std::size_t> shard_ids(const KeyRangeEnt& range) const;
    std::vector<std::size_t> shard_ids(const KeyRangeEnt& range,
                                        LockKind kind, LockMode mode) const;
    static bool same_range(const KeyRangeEnt& lhs, const KeyRangeEnt& rhs);
    static bool same_lock(const RangeLock& lhs, const RangeLock& rhs);
    std::array<Shard, kLocalLockShards> shards_;
    std::uint64_t bucket_width_{64};
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
