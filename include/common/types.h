#pragma once

#include <cstddef>
#include <cstdint>

namespace rrl {

// Names and values follow deneva so workload and transaction code can be
// migrated without a compatibility layer.
using UInt32 = std::uint32_t;
using UInt64 = std::uint64_t;
using txnid_t = std::uint64_t;
using txn_t = std::uint64_t;
using rid_t = std::uint64_t;
using idx_key_t = std::uint64_t;

// Keep the deneva return-code vocabulary, while keeping it scoped to rrl.
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

enum access_t {
    RD,
    WR,
    XP,
    SCAN,
};

enum lock_t {
    DLOCK_EX = 0,
    DLOCK_SH,
    LOCK_NONE,
};

enum class LockMode : std::uint8_t {
    S,
    X,
};

enum class LockKind : std::uint8_t {
    Record,
    Gap,
    NextKey,
    Predicate,
    Node,
};

inline bool lock_modes_conflict(LockMode lhs, LockMode rhs) {
    return lhs == LockMode::X || rhs == LockMode::X;
}

}  // namespace rrl