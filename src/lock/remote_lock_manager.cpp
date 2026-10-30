#include "lock/remote_lock_manager.h"

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace rrl {
namespace {

bool same_range(const KeyRangeEnt& lhs, const KeyRangeEnt& rhs) {
    return lhs.StartKey == rhs.StartKey && lhs.EndKey == rhs.EndKey &&
           lhs.has_start == rhs.has_start && lhs.has_end == rhs.has_end &&
           lhs.start_bound == rhs.start_bound && lhs.end_bound == rhs.end_bound;
}

}  // namespace

bool RemoteLockManager::connect(const std::string& host, std::uint16_t port) {
    if (!connection_.connect_client(host, port)) return false;
    const auto& remote = connection_.remote_memory();
    if (remote.length < sizeof(RemoteLockRegion)) return false;
    gate_address_ = remote.address + offsetof(RemoteLockRegion, gate);
    slots_address_ = remote.address + offsetof(RemoteLockRegion, slots);
    gate_rkey_ = slots_rkey_ = remote.rkey;
    region_image_.resize(sizeof(RemoteLockRegion));
    const auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    namespace_token_ = (now ^ reinterpret_cast<std::uintptr_t>(this)) & 0x0000ffffffffffffULL;
    if (namespace_token_ == 0) namespace_token_ = 1;
    owner_token_ = (namespace_token_ << 16) | 1ULL;
    return true;
}

bool RemoteLockManager::enter_gate() const {
    std::uint64_t previous = 0;
    return connection_.compare_swap(gate_address_, gate_rkey_, 0, owner_token_, previous) && previous == 0;
}

void RemoteLockManager::leave_gate() const {
    const std::uint64_t zero = 0;
    (void)connection_.write(gate_address_, gate_rkey_, &zero, sizeof(zero));
}

bool RemoteLockManager::load_slots(std::vector<RemoteLockSlot>& slots) const {
    slots.resize(kRemoteLockSlots);
    return connection_.read(slots_address_, slots_rkey_, slots.data(),
                            sizeof(RemoteLockSlot) * slots.size());
}

RangeLock RemoteLockManager::decode(const RemoteLockSlot& slot) {
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

void RemoteLockManager::encode(RemoteLockSlot& slot, std::uint64_t owner,
                               txnid_t txn_id, LockMode mode, LockKind kind,
                               const KeyRangeEnt& range) {
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

txnid_t RemoteLockManager::wire_txn(txnid_t txn_id) const {
    return (namespace_token_ << 16) ^ (txn_id & 0x0000ffffffffffffULL);
}

RC RemoteLockManager::lock(txnid_t txn_id, LockMode mode, const KeyRangeEnt& range,
                            LockKind kind, std::vector<txnid_t>* conflicts) {
    std::lock_guard<std::mutex> guard(io_mutex_);
    if (!connected() || range.empty() || !range.valid()) return ERROR;
    if (conflicts) conflicts->clear();
    if (!enter_gate()) return Abort;
    std::vector<RemoteLockSlot> slots;
    if (!load_slots(slots)) { leave_gate(); return ERROR; }

    const txnid_t id = wire_txn(txn_id);
    RangeLock request{id, mode, kind, range};
    std::size_t free_slot = kRemoteLockSlots;
    bool conflict = false;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].owner == 0) {
            if (free_slot == kRemoteLockSlots) free_slot = i;
            continue;
        }
        const RangeLock held = decode(slots[i]);
        if (request.conflicts(held)) {
            conflict = true;
            if (conflicts && std::find(conflicts->begin(), conflicts->end(), held.txn_id) == conflicts->end())
                conflicts->push_back(held.txn_id);
        }
    }
    if (conflict) { leave_gate(); return Abort; }

    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].owner != owner_token_ || slots[i].txn_id != id) continue;
        const RangeLock held = decode(slots[i]);
        if (held.kind == kind && same_range(held.range, range)) {
            slots[i].mode = static_cast<std::uint8_t>(lock_mode_join(held.mode, mode));
            const bool ok = connection_.write(slots_address_ + i * sizeof(RemoteLockSlot), slots_rkey_,
                                              &slots[i], sizeof(RemoteLockSlot));
            leave_gate(); return ok ? RCOK : ERROR;
        }
    }
    if (free_slot == kRemoteLockSlots) { leave_gate(); return Abort; }
    encode(slots[free_slot], owner_token_, id, mode, kind, range);
    const bool ok = connection_.write(slots_address_ + free_slot * sizeof(RemoteLockSlot), slots_rkey_,
                                      &slots[free_slot], sizeof(RemoteLockSlot));
    leave_gate();
    return ok ? RCOK : ERROR;
}

RC RemoteLockManager::unlock(txnid_t txn_id, const KeyRangeEnt& range) {
    std::lock_guard<std::mutex> guard(io_mutex_);
    if (!connected() || !enter_gate()) return Abort;
    std::vector<RemoteLockSlot> slots;
    if (!load_slots(slots)) { leave_gate(); return ERROR; }
    const txnid_t id = wire_txn(txn_id);
    bool removed = false;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].owner == owner_token_ && slots[i].txn_id == id &&
            same_range(decode(slots[i]).range, range)) {
            slots[i] = RemoteLockSlot{};
            if (!connection_.write(slots_address_ + i * sizeof(RemoteLockSlot), slots_rkey_,
                                   &slots[i], sizeof(RemoteLockSlot))) { leave_gate(); return ERROR; }
            removed = true;
        }
    }
    leave_gate();
    return removed ? RCOK : Abort;
}

RC RemoteLockManager::unlock_all(txnid_t txn_id) {
    std::lock_guard<std::mutex> guard(io_mutex_);
    if (!connected() || !enter_gate()) return Abort;
    std::vector<RemoteLockSlot> slots;
    if (!load_slots(slots)) { leave_gate(); return ERROR; }
    const txnid_t id = wire_txn(txn_id);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].owner == owner_token_ && slots[i].txn_id == id) {
            slots[i] = RemoteLockSlot{};
            if (!connection_.write(slots_address_ + i * sizeof(RemoteLockSlot), slots_rkey_,
                                   &slots[i], sizeof(RemoteLockSlot))) { leave_gate(); return ERROR; }
        }
    }
    leave_gate();
    return RCOK;
}

std::size_t RemoteLockManager::get_lock_count(txnid_t txn_id) const {
    std::lock_guard<std::mutex> guard(io_mutex_);
    if (!connected()) return 0;
    std::vector<RemoteLockSlot> slots;
    if (!load_slots(slots)) return 0;
    const txnid_t id = txn_id == 0 ? 0 : wire_txn(txn_id);
    std::size_t count = 0;
    for (const auto& slot : slots)
        if (slot.owner != 0 && (txn_id == 0 || (slot.owner == owner_token_ && slot.txn_id == id))) ++count;
    return count;
}

}  // namespace rrl