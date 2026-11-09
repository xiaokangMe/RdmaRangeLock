// Citron node layout adapted to (a) hardware without masked atomics and
// (b) shared/exclusive range locks.
//
// FAST'23 packs an internal node into one 64-bit word and uses masked-FAA /
// masked-CAS. rdma-core exposes only standard 8-byte CAS/FAA, so fields are
// split into independent words. The paper models a range mutex (exclusive
// only); the zongshu workload takes S locks for scans, so each node carries a
// writer and a reader half. With only X requests the writer half follows the
// paper exactly.
//
//   word 0  bitmapW   leaf writer bits (64 units)
//   word 1  bitmapR   leaf reader bits
//   word 2  TMax      ticket allocator            (FAA +1)
//   word 3  TCnt      served ticket               (FAA +1 on release)
//   word 4  OccW      writer occupancy (low 32) + Exp (bit 63)
//   word 5  OccR      reader occupancy
//   word 6  DMaxW     writer descendant notifications
//   word 7  DCntW     writer descendant releases
//   word 8  DMaxR     reader descendant notifications
//   word 9  DCntR     reader descendant releases
#pragma once

#include <cstdint>

namespace citron {

enum NodeWord : int {
    kBitmapW = 0,
    kBitmapR = 1,
    kTMax = 2,
    kTCnt = 3,
    kOccW = 4,
    kOccR = 5,
    kDMaxW = 6,
    kDCntW = 7,
    kDMaxR = 8,
    kDCntR = 9,
    kNodeWords = 10
};

constexpr std::uint64_t kOccCountMask = 0xFFFFFFFFull;
constexpr std::uint64_t kExpBit = 1ull << 63;
constexpr std::uint64_t kFieldMask16 = 0xFFFFull;

struct Node {
    std::uint64_t words[kNodeWords]{};
};

inline std::uint64_t node_tmax(const Node& n) {
    return n.words[kTMax];
}
inline std::uint64_t node_tcnt(const Node& n) {
    return n.words[kTCnt];
}
inline std::uint32_t node_occw(const Node& n) {
    return static_cast<std::uint32_t>(n.words[kOccW] & kOccCountMask);
}
inline std::uint32_t node_occr(const Node& n) {
    return static_cast<std::uint32_t>(n.words[kOccR] & kOccCountMask);
}
inline bool node_exp(const Node& n) { return (n.words[kOccW] & kExpBit) != 0; }
inline std::uint64_t node_dmax(const Node& n, bool writer) {
    return n.words[writer ? kDMaxW : kDMaxR];
}
inline std::uint64_t node_dcnt(const Node& n, bool writer) {
    return n.words[writer ? kDCntW : kDCntR];
}

// Bitmap mask of [l, r) inside a leaf covering [leaf_lo, leaf_hi).
inline std::uint64_t leaf_mask(std::uint64_t l, std::uint64_t r,
                               std::uint64_t leaf_lo, std::uint64_t leaf_hi) {
    const std::uint64_t lo = l > leaf_lo ? l : leaf_lo;
    const std::uint64_t hi = r < leaf_hi ? r : leaf_hi;
    if (hi <= lo) return 0;
    const std::uint64_t first = lo - leaf_lo;
    const std::uint64_t count = hi - lo;
    if (count >= 64) return ~0ull << first;
    return ((1ull << count) - 1) << first;
}

}  // namespace citron
