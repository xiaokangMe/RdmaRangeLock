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

enum class LockMode : std::uint8_t { IS, IX, S, X, SIX, U, IU, SIU, UIX };

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
    InsertIntent, // InnoDB-style insertion at one key, distinct from gap guard
    PriorKey,
};

// Gray1976 Table 1: IS/IX/S/SIX/X. Microsoft SQL Server locking guide,
// full compatibility chart: U/IU/SIU/UIX extensions (IU is page/bucket intent).
// Preserve the original four wire values; all new modes are appended.
inline bool valid_lock_mode(LockMode mode) {return mode<=LockMode::UIX;}
inline bool lock_mode_is_intent(LockMode mode) {
    return mode==LockMode::IS || mode==LockMode::IX || mode==LockMode::IU;
}
inline bool lock_modes_conflict(LockMode lhs, LockMode rhs) {
    // Rows and columns: IS IX S X SIX U IU SIU UIX.
    static constexpr bool compatible[9][9]={
        {1,1,1,0,1,1,1,1,1},
        {1,1,0,0,0,0,1,0,0},
        {1,0,1,0,0,1,1,1,0},
        {0,0,0,0,0,0,0,0,0},
        {1,0,0,0,0,0,1,0,0},
        {1,0,1,0,0,0,0,0,0},
        {1,1,1,0,1,0,1,1,0},
        {1,0,1,0,0,0,1,1,0},
        {1,0,0,0,0,0,0,0,0}
    };
    return !valid_lock_mode(lhs) || !valid_lock_mode(rhs) ||
        !compatible[static_cast<unsigned>(lhs)][static_cast<unsigned>(rhs)];
}
inline LockMode lock_mode_join(LockMode held, LockMode requested) {
    // Capability lattice: SIU=S+IU, SIX=S+IX, UIX=U+IX. This is a
    // permission join, not an ordering of enum values or compatibility rows.
    static constexpr unsigned capabilities[9]={1,7,9,63,15,27,3,11,31};
    if(!valid_lock_mode(held) || !valid_lock_mode(requested))return LockMode::X;
    const auto combined=capabilities[static_cast<unsigned>(held)] |
                        capabilities[static_cast<unsigned>(requested)];
    for(unsigned i=0;i<9;++i)if(capabilities[i]==combined)return static_cast<LockMode>(i);
    return LockMode::X;
}
inline LockMode lock_mode_record_cover(LockMode mode) {
    if(mode==LockMode::SIX || mode==LockMode::SIU)return LockMode::S;
    if(mode==LockMode::UIX)return LockMode::U;
    return mode;
}

}  // namespace rrl
