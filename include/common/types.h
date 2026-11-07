#pragma once

#include <cstddef>
#include <cstdint>

namespace rrl {

using UInt32 = std::uint32_t;
using UInt64 = std::uint64_t;
using txnid_t = std::uint64_t;
using txn_t = std::uint64_t;
using rid_t = std::uint64_t;
using idx_key_t = std::uint64_t;

enum RC {
    RCOK = 0,
    Commit,
    Abort,
    AbortConflict = Abort,
    AbortCapacity,
    AbortGate,
    WAIT,
    WAIT_REM,
    ERROR,
    FINISH,
    NONE,
};

enum access_t { RD, WR, XP, SCAN };
enum lock_t { DLOCK_EX = 0, DLOCK_SH, LOCK_NONE };

enum class LockMode : std::uint8_t { IS, IX, S, X };

enum class LockKind : std::uint8_t {
    Record,
    Gap,
    NextKey,
    Predicate,
    Node,
    // Compatibility tag for the same 1-D predicate-lock mechanism.
    // It does not define a second algorithm or a different representation.
    // Keep the wire value/cache identity for existing configs and results.
    // Provenance: docs/notes/paper-reproduction-audit-2026-11-07.md.
    ExactRange,
};

// Gray et al. (1976), Table 1 / requesting-node rules (PDF pp.4-5),
// restricted to IS/IX/S/X. IS and IX coexist; IX conflicts with S.
// Without SIX, S+IX conservatively joins to X (less concurrency, still safe).
// This is a resource-tree protocol, not necessarily the physical B+ tree.
// See docs/notes/paper-reproduction-audit-2026-11-07.md.
inline bool lock_modes_conflict(LockMode lhs, LockMode rhs) {
    if (lhs == LockMode::X || rhs == LockMode::X) return true;
    if ((lhs == LockMode::IX && rhs == LockMode::S) ||
        (lhs == LockMode::S && rhs == LockMode::IX)) return true;
    return false;
}

inline LockMode lock_mode_join(LockMode held, LockMode requested) {
    if (held == requested) return held;
    if (held == LockMode::X || requested == LockMode::X) return LockMode::X;
    if ((held == LockMode::S && requested == LockMode::IX) ||
        (held == LockMode::IX && requested == LockMode::S)) return LockMode::X;
    if (held == LockMode::S || requested == LockMode::S) return LockMode::S;
    if (held == LockMode::IX || requested == LockMode::IX) return LockMode::IX;
    return LockMode::IS;
}

}  // namespace rrl
