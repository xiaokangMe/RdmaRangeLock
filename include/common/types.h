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
};

// Multiple-granularity compatibility matrix. IS/IX are intention modes;
// they are compatible with each other and with shared modes as in Gray et al.
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
