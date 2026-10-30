#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "common/range.h"
#include "lock/lock_manager.h"
#include "rdma/rdma_transport.h"

namespace rrl {

constexpr std::size_t kRemoteLockShards = 16;
constexpr std::size_t kRemoteLockSlotsPerShard = 64;
constexpr std::size_t kRemoteLockSlots = kRemoteLockShards * kRemoteLockSlotsPerShard;

struct alignas(64) RemoteLockSlot {
    std::uint64_t owner{0};
    std::uint64_t txn_id{0};
    std::uint64_t start_key{0};
    std::uint64_t end_key{0};
    std::uint8_t mode{0};
    std::uint8_t kind{0};
    std::uint8_t has_start{0};
    std::uint8_t has_end{0};
    std::uint8_t start_bound{0};
    std::uint8_t end_bound{0};
    std::uint8_t reserved[6]{};
};
static_assert(sizeof(RemoteLockSlot) == 64, "remote lock slot must be cache-line sized");

struct alignas(64) RemoteLockShard {
    std::uint64_t gate{0};
    std::uint64_t active_slots{0};
    std::uint8_t reserved[48]{};
    RemoteLockSlot slots[kRemoteLockSlotsPerShard]{};
};

struct alignas(64) RemoteLockRegion {
    RemoteLockShard shards[kRemoteLockShards]{};
};

class RemoteLockManager final : public IRangeLockManager {
public:
    RemoteLockManager() = default;
    ~RemoteLockManager() override = default;
    RemoteLockManager(const RemoteLockManager&) = delete;
    RemoteLockManager& operator=(const RemoteLockManager&) = delete;

    bool connect(const std::string& host, std::uint16_t port);
    void set_bucket_width(std::uint64_t width);
    bool connected() const { return connection_.connected(); }
    RC lock(txnid_t txn_id, LockMode mode, const KeyRangeEnt& range,
            LockKind kind = LockKind::Predicate,
            std::vector<txnid_t>* conflicts = nullptr) override;
    RC unlock(txnid_t txn_id, const KeyRangeEnt& range,
              LockKind kind = LockKind::Predicate) override;
    RC unlock_all(txnid_t txn_id) override;
    std::size_t get_lock_count(txnid_t txn_id = 0) const override;

private:
    std::vector<std::size_t> shard_ids(const KeyRangeEnt& range) const;
    bool enter_shards(const std::vector<std::size_t>& ids,
                      std::vector<std::size_t>& acquired) const;
    void leave_shards(const std::vector<std::size_t>& acquired) const;
    bool load_slots(std::size_t shard, std::vector<RemoteLockSlot>& slots) const;
    bool store_active_slots(std::size_t shard, std::uint64_t count) const;
    std::uint64_t shard_base(std::size_t shard) const;
    static RangeLock decode(const RemoteLockSlot& slot);
    static void encode(RemoteLockSlot& slot, std::uint64_t owner,
                       txnid_t txn_id, LockMode mode, LockKind kind,
                       const KeyRangeEnt& range);
    txnid_t wire_txn(txnid_t txn_id) const;

    mutable std::mutex io_mutex_;
    mutable RdmaConnection connection_;
    std::uint64_t shards_address_{0};
    std::uint32_t shards_rkey_{0};
    std::uint64_t bucket_width_{64};
    std::uint64_t owner_token_{0};
    std::uint64_t namespace_token_{0};
};

}  // namespace rrl
