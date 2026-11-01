#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "common/range.h"
#include "lock/remote_lock_slot.h"

namespace rrl {

// Ordered view over the slots of one shard.
//
// Conflict detection used to be a linear scan of every published slot. Sorting
// the intervals by start key and keeping a running maximum of their end keys
// reduces a range request to two binary searches plus a scan over the intervals
// that can actually overlap it. The prefix maximum is what makes the left bound
// safe: it is monotone, so every interval before the first one whose prefix
// maximum reaches the query start ends below that start.
class SlotIndex {
public:
    static constexpr std::uint64_t kMaxKey = std::numeric_limits<std::uint64_t>::max();

    void build(const std::vector<RemoteLockSlot>& slots) {
        intervals_.clear();
        starts_.clear();
        max_end_.clear();
        unbounded_.clear();
        intervals_.reserve(slots.size());
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].has_start) intervals_.push_back(i);
            else unbounded_.push_back(i);
        }
        std::sort(intervals_.begin(), intervals_.end(),
                  [&](std::size_t lhs, std::size_t rhs) {
                      return slots[lhs].start_key < slots[rhs].start_key;
                  });
        starts_.reserve(intervals_.size());
        max_end_.reserve(intervals_.size());
        std::uint64_t running = 0;
        for (const std::size_t index : intervals_) {
            starts_.push_back(slots[index].start_key);
            running = std::max(running, end_of(slots[index]));
            max_end_.push_back(running);
        }
    }

    // Candidate window [first, last) for a query interval [qs, qe].
    std::pair<std::size_t, std::size_t> window(std::uint64_t qs, std::uint64_t qe) const {
        const auto last = static_cast<std::size_t>(
            std::upper_bound(starts_.begin(), starts_.end(), qe) - starts_.begin());
        const auto first = static_cast<std::size_t>(
            std::lower_bound(max_end_.begin(), max_end_.end(), qs) - max_end_.begin());
        return {std::min(first, last), last};
    }

    // Finds the slot this transaction already published for exactly this range.
    bool find_owned(const std::vector<RemoteLockSlot>& slots, txnid_t txn, LockKind kind,
                    const KeyRangeEnt& range, std::size_t& out) const {
        const auto bounds = window(query_start(range), query_end(range));
        for (std::size_t i = bounds.first; i < bounds.second; ++i) {
            if (matches(slots[intervals_[i]], txn, kind, range)) { out = intervals_[i]; return true; }
        }
        for (const std::size_t index : unbounded_)
            if (matches(slots[index], txn, kind, range)) { out = index; return true; }
        return false;
    }

    const std::vector<std::size_t>& intervals() const { return intervals_; }
    const std::vector<std::size_t>& unbounded() const { return unbounded_; }

    static std::uint64_t query_start(const KeyRangeEnt& range) {
        return range.has_start ? range.StartKey : 0;
    }
    static std::uint64_t query_end(const KeyRangeEnt& range) {
        return range.has_end ? range.EndKey : kMaxKey;
    }

private:
    static std::uint64_t end_of(const RemoteLockSlot& slot) {
        return slot.has_end ? slot.end_key : kMaxKey;
    }
    static bool matches(const RemoteLockSlot& slot, txnid_t txn, LockKind kind,
                        const KeyRangeEnt& range) {
        if (slot.txn_id != txn || slot.kind != static_cast<std::uint8_t>(kind)) return false;
        const auto held = decode_remote_slot(slot).range;
        return held.StartKey == range.StartKey && held.EndKey == range.EndKey &&
               held.has_start == range.has_start && held.has_end == range.has_end &&
               held.start_bound == range.start_bound && held.end_bound == range.end_bound;
    }

    std::vector<std::size_t> intervals_;
    std::vector<std::uint64_t> starts_;
    std::vector<std::uint64_t> max_end_;
    std::vector<std::size_t> unbounded_;
};

}  // namespace rrl
