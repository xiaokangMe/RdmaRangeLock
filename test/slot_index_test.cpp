#include "lock/slot_index.h"

#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

using namespace rrl;

namespace {

RangeLock random_lock(std::mt19937_64& random, uint64_t span) {
    RangeLock lock;
    lock.txn_id = 1 + random() % 8;
    lock.mode = static_cast<LockMode>(random() % 4);
    lock.kind = static_cast<LockKind>(random() % 5);
    KeyRangeEnt range;
    switch (random() % 4) {
        case 0: {
            const auto key = random() % span;
            range = KeyRangeEnt::point(key);
            break;
        }
        case 1: {
            auto left = random() % span;
            auto right = random() % span;
            if (left > right) std::swap(left, right);
            range = KeyRangeEnt::closed(left, right);
            break;
        }
        case 2: {
            auto left = random() % span;
            auto right = random() % span;
            if (left > right) std::swap(left, right);
            range = KeyRangeEnt::half_open(left, right + 1);
            break;
        }
        default:
            range = KeyRangeEnt::unbounded();
            break;
    }
    lock.range = range;
    return lock;
}

// Exactly the scan the lock manager performs on a loaded shard.
bool indexed_conflict(const SlotIndex& index, const std::vector<RemoteLockSlot>& slots,
                      const RangeLock& request) {
    bool result = false;
    index.for_each_candidate(slots, request.range, [&](const RemoteLockSlot& slot) {
        if (request.conflicts(decode_remote_slot(slot))) result = true;
    });
    return result;
}

bool brute_force_conflict(const std::vector<RemoteLockSlot>& slots, const RangeLock& request) {
    for (const auto& slot : slots)
        if (request.conflicts(decode_remote_slot(slot))) return true;
    return false;
}

}  // namespace

int main() {
    std::mt19937_64 random(20261101);
    constexpr std::uint64_t kSpan = 5000;
    for (int round = 0; round < 200; ++round) {
        std::vector<RemoteLockSlot> slots;
        slots.reserve(1 + round % 300);
        for (std::size_t i = 0; i < 1 + static_cast<std::size_t>(round % 300); ++i) {
            RemoteLockSlot slot;
            const RangeLock lock = random_lock(random, kSpan);
            encode_remote_slot(slot, 1, lock.txn_id, lock.mode, lock.kind, lock.range);
            slots.push_back(slot);
        }
        SlotIndex index;
        index.build(slots);
        SlotIndex linear;
        linear.build(slots, true);
        for (int query = 0; query < 200; ++query) {
            const RangeLock request = query % 4 == 0
                ? decode_remote_slot(slots[static_cast<std::size_t>(query) % slots.size()])
                : random_lock(random, kSpan);
            const bool expected = brute_force_conflict(slots, request);
            const bool actual = indexed_conflict(index, slots, request);
            if (expected != actual || expected != indexed_conflict(linear, slots, request)) {
                std::cerr << "FAIL: conflict mismatch round=" << round << " query=" << query
                          << " expected=" << expected << " actual=" << actual
                          << " range=" << request.range.to_string() << "\n";
                return 1;
            }
            // find_owned must agree with a linear scan over the same slots.
            std::size_t expected_slot = slots.size();
            for (std::size_t i = 0; i < slots.size(); ++i) {
                const auto held = decode_remote_slot(slots[i]);
                if (held.txn_id == request.txn_id && held.kind == request.kind &&
                    held.range.StartKey == request.range.StartKey &&
                    held.range.EndKey == request.range.EndKey &&
                    held.range.has_start == request.range.has_start &&
                    held.range.has_end == request.range.has_end &&
                    held.range.start_bound == request.range.start_bound &&
                    held.range.end_bound == request.range.end_bound) {
                    expected_slot = i;
                    break;
                }
            }
            std::size_t actual_slot = 0;
            const bool found = index.find_owned(slots, request.txn_id, request.kind, request.range,
                                                actual_slot);
            std::size_t linear_slot = 0;
            const bool linear_found = linear.find_owned(slots, request.txn_id, request.kind,
                                                         request.range, linear_slot);
            if (found != (expected_slot != slots.size()) || found != linear_found ||
                (linear_found && linear_slot != expected_slot)) {
                std::cerr << "FAIL: find_owned presence mismatch round=" << round << "\n";
                return 1;
            }
            if (found && !(decode_remote_slot(slots[actual_slot]).range.StartKey ==
                           request.range.StartKey)) {
                std::cerr << "FAIL: find_owned returned a different interval\n";
                return 1;
            }
        }
    }
    std::cout << "slot index: OK\n";
    return 0;
}
