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
#include "lock/txn_lock_cache.h"
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
        // Gate acquisitions that failed on the transport rather than on a busy
        // gate; these are the ones the caller must treat as a hard error.
        std::uint64_t gate_transport_errors{0};
        // get_lock_count retains its size_t contract. A zero is valid only if
        // this monotonic counter did not change during the observation.
        std::uint64_t count_errors{0};
        RC last_count_error{RCOK};
        std::uint64_t local_errors{0};
        bool profile_enabled{false};
        bool slot_lookup_linear{false};
        std::uint64_t lock_calls{0}, cache_hits{0};
        std::uint64_t io_wait_ns{0}, local_gate_wait_ns{0}, gate_hold_ns{0};
        std::uint64_t index_build_ns{0}, lookup_ns{0}, slot_checks{0};
        std::uint64_t sync_calls{0}, sync_ns{0}, read_bytes{0}, write_bytes{0};
        std::array<std::uint64_t, 6> phase_calls{}, phase_ns{};
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
    // Returned by value: handing out a reference into a mutex-protected record
    // is a race as soon as another thread overwrites it.
    RdmaFailureInfo last_failure() const;
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
    friend struct RemoteLockManagerTestPeer;
    void record_count_error(RC rc) const;
    // One work queue, one completion queue and one scratch buffer: a connection
    // can only serve one thread at a time. When fewer connections than threads
    // are requested, callers queue here instead of corrupting each other's
    // buffers; with one connection per thread the mutex is uncontended.
    struct Workspace;
    struct ConnectionSlot {
        ConnectionSlot();
        ~ConnectionSlot();
        std::unique_ptr<RdmaConnection> connection;
        std::unique_ptr<Workspace> workspace;
        std::uint64_t owner_token{0};
        mutable std::mutex io_mutex;
    };
    struct LoadedShard;
    class LocalGateGuard;
    class RemoteGateGuard;
    RC lock_impl(txnid_t, LockMode, const KeyRangeEnt&, LockKind, std::vector<txnid_t>*);
    RC release_impl(txnid_t, const KeyRangeEnt*, LockKind);
    std::size_t count_impl(txnid_t) const;
    void invalidate(ConnectionSlot* slot) const noexcept;
    // Used only by the friend regression test, never configured at runtime.
    void (*after_gates_test_hook_)() = nullptr;
    void (*gate_state_test_hook_)(bool) = nullptr;

    // A gate that is busy and a gate whose CAS did not complete are different
    // failures: the first is contention the caller may retry, the second means
    // the transport is broken and ownership needs peer recovery.
    enum class GateOutcome : std::uint8_t { Acquired, Busy, TransportError };

    std::vector<std::size_t> shard_ids(const KeyRangeEnt& range) const;
    std::vector<std::size_t> shard_ids(const KeyRangeEnt& range,
                                       LockKind kind, LockMode mode) const;
    std::vector<std::size_t> shards_for_txn(txnid_t txn_id) const;
    // Rebuilds the shard set from the locks still cached, so a single unlock
    // stops the release path from visiting shards that hold nothing.
    void refresh_txn_shards(txnid_t txn_id) const;
    // reliable=false keeps the no-wait acquisition semantics used by lock().
    // reliable=true waits with backoff until the gate is free, which is what a
    // release path needs: abandoning a release leaks the locks behind it.
    GateOutcome enter_shards(const std::vector<std::size_t>& ids,
                             std::vector<std::size_t>& acquired, bool reliable = false) const;
    GateOutcome try_enter_shards(const std::vector<std::size_t>& ids,
                                 std::vector<std::size_t>& acquired) const;
    bool leave_shards(const std::vector<std::size_t>& acquired) const;
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
    bool local_gate_enabled_{true};
    bool profile_enabled_{false};
    // Full snapshot build+lookup measurements favor a scan for this backend.
    // RRL_SLOT_LOOKUP=index retains the explicit comparison path.
    bool slot_lookup_linear_{true};
    mutable std::atomic<std::uint64_t> lock_calls_{0}, cache_hits_{0};
    mutable std::atomic<std::uint64_t> io_wait_ns_{0}, local_gate_wait_ns_{0}, gate_hold_ns_{0};
    mutable std::atomic<std::uint64_t> index_build_ns_{0}, lookup_ns_{0}, slot_checks_{0};
    mutable std::atomic<std::uint64_t> gate_attempts_{0};
    mutable std::atomic<std::uint64_t> gate_retry_count_{0};
    mutable std::atomic<std::uint64_t> gate_abort_count_{0};
    mutable std::atomic<std::uint64_t> batch_count_{0};
    mutable std::atomic<std::uint64_t> gate_transport_errors_{0};
    mutable std::atomic<std::uint64_t> count_errors_{0};
    mutable std::atomic<RC> last_count_error_{RCOK};
    mutable std::atomic<std::uint64_t> local_errors_{0};
    mutable std::atomic<bool> invalid_{false};
    // Local gate serialisation. The remote CAS gate is already the correctness
    // boundary, so this is purely a performance knob: waiting on an in-process
    // mutex is far cheaper than losing a CAS round trip and retrying. It is on
    // by default and can be disabled with RRL_LOCAL_GATE=0 for A/B runs.
    mutable std::array<std::mutex, kRemoteLockShards> gate_mutexes_;
    mutable TxnLockCache txn_cache_;
};

}  // namespace rrl
