#pragma once

#include <cstddef>
#include <cstdint>

#include "common/range.h"

namespace rrl {

constexpr std::size_t kRemoteLockShards = 16;
constexpr std::size_t kRemoteLockSlotsPerShard = 1024;
constexpr std::size_t kRemoteLockSlots = kRemoteLockShards * kRemoteLockSlotsPerShard;

// One published lock. Kept cache-line sized so a slot read or write is a single
// aligned transfer.
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

inline RangeLock decode_remote_slot(const RemoteLockSlot& slot) {
    KeyRangeEnt range;
    range.StartKey = slot.start_key;
    range.EndKey = slot.end_key;
    range.has_start = slot.has_start != 0;
    range.has_end = slot.has_end != 0;
    range.start_bound = slot.start_bound ? Bound::Closed : Bound::Open;
    range.end_bound = slot.end_bound ? Bound::Closed : Bound::Open;
    return RangeLock{slot.txn_id, static_cast<LockMode>(slot.mode),
                     static_cast<LockKind>(slot.kind), range};
}

inline void encode_remote_slot(RemoteLockSlot& slot, std::uint64_t owner, txnid_t txn_id,
                               LockMode mode, LockKind kind, const KeyRangeEnt& range) {
    slot = RemoteLockSlot{};
    slot.owner = owner;
    slot.txn_id = txn_id;
    slot.start_key = range.StartKey;
    slot.end_key = range.EndKey;
    slot.mode = static_cast<std::uint8_t>(mode);
    slot.kind = static_cast<std::uint8_t>(kind);
    slot.has_start = range.has_start ? 1 : 0;
    slot.has_end = range.has_end ? 1 : 0;
    slot.start_bound = range.start_bound == Bound::Closed ? 1 : 0;
    slot.end_bound = range.end_bound == Bound::Closed ? 1 : 0;
}

}  // namespace rrl
