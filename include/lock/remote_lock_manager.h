#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/range.h"
#include "lock/lock_manager.h"
#include "lock/remote_lock_slot.h"
#include "lock/slot_index.h"
#include "rdma/rdma_transport.h"

namespace rrl {

// Per-transaction bookkeeping is split across caches so that a manager-wide
// mutex is not on the path of every lock request.
constexpr std::size_t kRemoteTxnCacheShards = 64;
// One work request batch. Sized to cover every shard touched by one range
// request without falling back to a second round trip.
constexpr std::size_t kRemoteBatchLimit = 128;

class RemoteLockManager final : public IRangeLockManager {
public:
    RemoteLockManager();
    ~RemoteLockManager() override;
    RemoteLockManager(const RemoteLockManager&) = delete;
    RemoteLockManager& operator=(const RemoteLockManager&) = delete;

    struct Diagnostics {
        std::uint64_t gate_attempts{0};
        std::uint64_t gate_retries{0};
        std::uint64_t gate_aborts{0};
        // Round trips saved: each batch call carries several work requests in
        // one post, so this counts the batched calls rather than single ops.
        std::uint64_t batches{0};
        std::uint64_t local_gate_waits{0};
    };

    bool connect(const std::string& host, std::uint16_t port);
    void set_bucket_width(std::uint64_t width);
    void set_connection_count(std::size_t count);
    bool connected() const;
    std::uint64_t rdma_timeouts() const;
    std::uint64_t rdma_completion_errors() const;
    std::uint64_t rdma_operations() const;
    Diagnostics diagnostics() const;
    // Details of the most recent failed work request: opcode, shard, WR index,
    // QP and phase tag, so a one-second stall can be localised.
    const RdmaFailureInfo& last_failure() const;
    std::int64_t last_failure_shard() const;
    std::uint64_t timeouts_by_tag(RdmaOpTag tag) const;
    RC lock(txnid_t txn_id, LockMode mode, const KeyRangeEnt& range,
            LockKind kind = LockKind::Predicate,
            std::vector<txnid_t>* conflicts = nullptr) override;
    RC unlock(txnid_t txn_id, const KeyRangeEnt& range,
              LockKind kind = LockKind::Predicate) override;
    RC unlock_all(txnid_t txn_id) override;
    std::size_t get_lock_count(txnid_t txn_id = 0) const override;

private:
    struct ConnectionSlot;
    struct CachedLock {
        LockMode mode{LockMode::IS};
        LockKind kind{LockKind::Predicate};
        KeyRangeEnt range{};
    };
    struct LoadedShard;
    struct TxnCacheShard {
        mutable std::mutex mutex;
        std::unordered_map<txnid_t, std::vector<std::size_t>> shards;
        std::unordered_map<txnid_t, std::vector<CachedLock>> locks;
    };
    class LocalGateGuard;

    std::vector<std::size_t> shard_ids(const KeyRangeEnt& range) const;
    std::vector<std::size_t> shard_ids(const KeyRangeEnt& range,
                                       LockKind kind, LockMode mode) const;
    TxnCacheShard& cache_shard(txnid_t txn_id) const;
    void remember_shards(txnid_t txn_id, const std::vector<std::size_t>& ids) const;
    std::vector<std::size_t> shards_for_txn(txnid_t txn_id) const;
    bool cached_lock_satisfies(txnid_t txn_id, LockMode mode,
                               const KeyRangeEnt& range, LockKind kind) const;
    void remember_lock(txnid_t txn_id, LockMode mode,
                       const KeyRangeEnt& range, LockKind kind) const;
    void forget_lock(txnid_t txn_id, const KeyRangeEnt& range, LockKind kind) const;
    std::size_t cached_lock_count(txnid_t txn_id) const;
    void forget_shard(txnid_t txn_id, std::size_t shard) const;
    void drop_cache(txnid_t txn_id) const;
    // reliable=false keeps the no-wait acquisition semantics used by lock().
    // reliable=true waits with backoff until the gate is free, which is what a
    // release path needs: abandoning a release leaks the locks behind it.
    bool enter_shards(const std::vector<std::size_t>& ids,
                      std::vector<std::size_t>& acquired, bool reliable = false) const;
    bool try_enter_shards(const std::vector<std::size_t>& ids,
                          std::vector<std::size_t>& acquired) const;
    void leave_shards(const std::vector<std::size_t>& acquired) const;
    bool load_tables(const std::vector<std::size_t>& ids,
                     std::vector<LoadedShard>& tables) const;
    bool write_chunks(const std::vector<RdmaWriteRequest>& writes) const;
    // Shared implementation of unlock() and unlock_all(): one gate round trip,
    // one batched read, one batched write, one batched release per call.
    RC release(txnid_t txn_id, const KeyRangeEnt* range, LockKind kind);
    std::uint64_t shard_base(std::size_t shard) const;
    std::uint32_t current_shards_rkey() const;
    ConnectionSlot& current_slot() const;
    RdmaConnection& current_connection() const;
    std::uint64_t current_owner_token() const;
    std::uint64_t current_namespace_token() const;
    bool owns_owner_token(std::uint64_t owner) const;
    static RangeLock decode(const RemoteLockSlot& slot);
    static void encode(RemoteLockSlot& slot, std::uint64_t owner,
                       txnid_t txn_id, LockMode mode, LockKind kind,
                       const KeyRangeEnt& range);
    txnid_t wire_txn(txnid_t txn_id) const;

    mutable std::vector<std::unique_ptr<ConnectionSlot>> connections_;
    std::size_t connection_count_{1};
    mutable std::atomic<std::size_t> next_connection_{0};
    std::uint64_t connection_generation_{0};
    std::uint64_t shards_address_{0};
    std::uint64_t bucket_width_{64};
    std::uint64_t namespace_token_{0};
    std::size_t gate_retries_{64};
    bool local_gate_enabled_{false};
    mutable std::atomic<std::uint64_t> gate_attempts_{0};
    mutable std::atomic<std::uint64_t> gate_retry_count_{0};
    mutable std::atomic<std::uint64_t> gate_abort_count_{0};
    mutable std::atomic<std::uint64_t> batch_count_{0};
    // Optional local gate serialisation. The remote CAS gate is already the
    // correctness boundary; this only reduces failed acquisitions, so it is off
    // by default and kept switchable for A/B measurement.
    mutable std::array<std::mutex, kRemoteLockShards> gate_mutexes_;
    mutable std::array<TxnCacheShard, kRemoteTxnCacheShards> txn_cache_;
};

}  // namespace rrl
