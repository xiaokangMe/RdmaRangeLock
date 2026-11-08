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
    bool is_unbounded() const { return !has_start && !has_end; }

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

// Jordan1981 section 4.1: Lu contains actual tuples, never write ranges.
inline bool valid_precision_request(LockMode mode, LockKind kind, const KeyRangeEnt& range) {
    if(kind==LockKind::PrecisionPredicate || kind==LockKind::GriffinPredicate)return mode==LockMode::S;
    if(kind==LockKind::PrecisionUpdate || kind==LockKind::GriffinUpdate)
        return mode==LockMode::X && range.has_start && range.has_end &&
            range.StartKey==range.EndKey && range.start_bound==Bound::Closed && range.end_bound==Bound::Closed;
    return true;
}

struct RangeLock {
    txnid_t txn_id{0};
    LockMode mode{LockMode::S};
    LockKind kind{LockKind::Predicate};
    KeyRangeEnt range{};

    // Eswaran et al. (CACM 1976), section 3, condition (14):
    // same relation + at least one writer + jointly satisfiable predicates.
    // Here the language is a single ordered-key interval; all records share
    // one relation. Node intentions below implement the separate MGL subset.
    // LockKind labels for non-Node locks do not isolate conflict namespaces.
    bool conflicts(const RangeLock& other) const {
        if(txn_id==other.txn_id)return false;
        const auto predicate=[](LockKind k) {
            return k==LockKind::PrecisionPredicate || k==LockKind::GriffinPredicate;
        };
        const auto update=[](LockKind k) {
            return k==LockKind::PrecisionUpdate || k==LockKind::GriffinUpdate;
        };
        if((predicate(kind) || update(kind)) && (predicate(other.kind) || update(other.kind))) {
            // Lp x Lu only. Neither Lp/Lp satisfiability nor Lu/Lu checking.
            if(predicate(kind)==predicate(other.kind))return false;
            const auto& p=predicate(kind)?range:other.range;
            const auto& tuple=update(kind)?range:other.range;
            return p.contains(tuple.StartKey);
        }
        const auto griffin=[](LockKind k) {
            return k==LockKind::GriffinPredicate || k==LockKind::GriffinUpdate;
        };
        // Griffin protects key membership; record locks protect contents.
        // A value-only update cannot cause a phantom (Griffin §V-A).
        if((griffin(kind) && other.kind==LockKind::Record) ||
           (griffin(other.kind) && kind==LockKind::Record))return false;
        if (!range.overlaps(other.range) || !lock_modes_conflict(mode, other.mode)) return false;
        // MySQL 8.4 17.7.1: gap guards are purely inhibitive.
        // They coexist regardless of S/X and do not lock existing records.
        // InsertIntent reserves an actual insert position; distinct positions
        // coexist through ordinary geometric point intersection.
        if(kind==LockKind::Gap || other.kind==LockKind::Gap) {
            if(kind==LockKind::Gap && other.kind==LockKind::Gap)return false;
            const auto counterpart=kind==LockKind::Gap?other.kind:kind;
            if(counterpart==LockKind::Record)return false;
        }
        const bool this_parent_intention = kind == LockKind::Node &&
            range.is_unbounded() && lock_mode_is_intent(mode);
        const bool other_parent_intention = other.kind == LockKind::Node &&
            other.range.is_unbounded() &&
            lock_mode_is_intent(other.mode);
        if (this_parent_intention && !other.range.is_unbounded()) return false;
        if (other_parent_intention && !range.is_unbounded()) return false;
        // A bucket intention announces descendant access; it does not lock
        // every record in that bucket. Bucket S/X versus record locks still
        // conflicts, as do bucket S versus bucket IX and record S versus X.
        const auto bucket_intention = [](const RangeLock& a, const RangeLock& b) {
            return a.kind == LockKind::Node && (b.kind == LockKind::Record || b.kind==LockKind::HierarchyPredicate) &&
                lock_mode_is_intent(a.mode);
        };
        if (bucket_intention(*this, other) || bucket_intention(other, *this)) return false;
        // Composite parent intent is not an additional conflicting row
        // lock. Only its covering S/U/X component protects descendants.
        if(kind==LockKind::Node && (other.kind==LockKind::Record || other.kind==LockKind::HierarchyPredicate))
            return lock_modes_conflict(lock_mode_record_cover(mode),other.mode);
        if((kind==LockKind::Record || kind==LockKind::HierarchyPredicate) && other.kind==LockKind::Node)
            return lock_modes_conflict(mode,lock_mode_record_cover(other.mode));
        return true;
    }
};

}  // namespace rrl
