#pragma once

#include <cstdint>
#include <limits>
#include <string>

#include "common/types.h"

namespace rrl {

enum class Bound : std::uint8_t { Open, Closed };

// A key interval with explicit endpoint presence and inclusiveness.
// The two-argument constructor preserves deneva's EndKey==0 point convention.
class KeyRangeEnt {
public:
    idx_key_t StartKey{0};
    idx_key_t EndKey{0};
    bool has_start{false};
    bool has_end{false};
    Bound start_bound{Bound::Closed};
    Bound end_bound{Bound::Open};

    KeyRangeEnt() = default;
    KeyRangeEnt(idx_key_t start, idx_key_t end)
        : StartKey(start), EndKey(end == 0 ? start : end), has_start(true), has_end(true),
          start_bound(Bound::Closed), end_bound(end == 0 ? Bound::Closed : Bound::Open) {}

    static KeyRangeEnt point(idx_key_t key) {
        KeyRangeEnt r; r.StartKey = key; r.EndKey = key; r.has_start = r.has_end = true;
        r.start_bound = Bound::Closed; r.end_bound = Bound::Closed; return r;
    }
    static KeyRangeEnt closed(idx_key_t start, idx_key_t end) {
        KeyRangeEnt r; r.StartKey = start; r.EndKey = end; r.has_start = r.has_end = true;
        r.start_bound = Bound::Closed; r.end_bound = Bound::Closed; return r;
    }
    static KeyRangeEnt half_open(idx_key_t start, idx_key_t end) {
        KeyRangeEnt r; r.StartKey = start; r.EndKey = end; r.has_start = r.has_end = true;
        r.start_bound = Bound::Closed; r.end_bound = Bound::Open; return r;
    }
    static KeyRangeEnt open(idx_key_t start, idx_key_t end) {
        KeyRangeEnt r; r.StartKey = start; r.EndKey = end; r.has_start = r.has_end = true;
        r.start_bound = Bound::Open; r.end_bound = Bound::Open; return r;
    }
    static KeyRangeEnt from_left(idx_key_t start, Bound bound = Bound::Closed) {
        KeyRangeEnt r; r.StartKey = start; r.has_start = true; r.start_bound = bound; return r;
    }
    static KeyRangeEnt to_right(idx_key_t end, Bound bound = Bound::Open) {
        KeyRangeEnt r; r.EndKey = end; r.has_end = true; r.end_bound = bound; return r;
    }
    static KeyRangeEnt unbounded() { return KeyRangeEnt(); }

    bool valid() const {
        if (!has_start || !has_end) return true;
        if (StartKey < EndKey) return true;
        if (StartKey > EndKey) return false;
        return start_bound == Bound::Closed && end_bound == Bound::Closed;
    }
    bool empty() const {
        if (!valid()) return true;
        return has_start && has_end && StartKey == EndKey &&
               (start_bound == Bound::Open || end_bound == Bound::Open);
    }
    bool contains(idx_key_t key) const {
        if (empty()) return false;
        if (has_start && (key < StartKey || (key == StartKey && start_bound == Bound::Open))) return false;
        if (has_end && (key > EndKey || (key == EndKey && end_bound == Bound::Open))) return false;
        return true;
    }
    bool getOverlaps(const KeyRangeEnt& other) const {
        if (empty() || other.empty()) return false;
        return !ends_before(*this, other) && !ends_before(other, *this);
    }
    bool overlaps(const KeyRangeEnt& other) const { return getOverlaps(other); }
    std::string to_string() const {
        std::string result;
        result += has_start && start_bound == Bound::Closed ? "[" : "(";
        result += has_start ? std::to_string(StartKey) : "-inf";
        result += ",";
        result += has_end ? std::to_string(EndKey) : "+inf";
        result += has_end && end_bound == Bound::Closed ? "]" : ")";
        return result;
    }
private:
    static bool ends_before(const KeyRangeEnt& lhs, const KeyRangeEnt& rhs) {
        if (!lhs.has_end || !rhs.has_start) return false;
        if (lhs.EndKey < rhs.StartKey) return true;
        if (lhs.EndKey > rhs.StartKey) return false;
        return lhs.end_bound == Bound::Open || rhs.start_bound == Bound::Open;
    }
};

struct RangeLock {
    txnid_t txn_id{0};
    LockMode mode{LockMode::S};
    LockKind kind{LockKind::Predicate};
    KeyRangeEnt range{};

    bool conflicts(const RangeLock& other) const {
        if (txn_id == other.txn_id || !range.overlaps(other.range) ||
            !lock_modes_conflict(mode, other.mode)) return false;
        const bool this_parent_intention = kind == LockKind::Node &&
            !range.has_start && !range.has_end &&
            (mode == LockMode::IS || mode == LockMode::IX);
        const bool other_parent_intention = other.kind == LockKind::Node &&
            !other.range.has_start && !other.range.has_end &&
            (other.mode == LockMode::IS || other.mode == LockMode::IX);
        if (this_parent_intention && other.range.has_start) return false;
        if (other_parent_intention && range.has_start) return false;
        return true;
    }
};

}  // namespace rrl