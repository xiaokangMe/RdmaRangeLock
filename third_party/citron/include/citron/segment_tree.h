// Citron's lock tree: a quaternary segment tree stored in a flat array by
// level order (FAST'23 §3.3). Node index arithmetic is pointer-free:
//   Child(x, i) = 4x - 2 + i     (i = 0..3)
//   Parent(x)   = (x + 2) / 4
// Leaves cover 64 address units each, which is exactly one 64-bit bitmap.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace citron {

class SegmentTree {
public:
    struct NodeRange {
        std::uint64_t index{0};
        std::uint64_t lo{0};
        std::uint64_t hi{0};
        bool leaf{false};
    };

    // units = 64 * 4^height. height is the number of internal levels.
    explicit SegmentTree(int height);

    int height() const { return height_; }
    std::uint64_t units() const { return units_; }
    std::uint64_t first_leaf_index() const { return level_start_[height_]; }
    std::uint64_t total_nodes() const { return level_start_[height_ + 1] - 1; }

    bool is_leaf(std::uint64_t index) const { return index >= first_leaf_index(); }
    int depth_of(std::uint64_t index) const;
    std::uint64_t level_start(int depth) const { return level_start_[depth]; }
    std::uint64_t level_count(int depth) const;

    static std::uint64_t child(std::uint64_t index, int i) {
        return 4 * index - 2 + static_cast<std::uint64_t>(i);
    }
    static std::uint64_t parent(std::uint64_t index) { return (index + 2) / 4; }

    NodeRange node(std::uint64_t index) const;
    // Ancestors from parent up to the root (empty for the root).
    std::vector<std::uint64_t> ancestors(std::uint64_t index) const;

    // Cover [l, r) with at most k nodes, minimising covered-but-unrequested
    // units (FAST'23 §3.5.1 tree knapsack). Leaves are exact: the bitmap can
    // mask any sub-range inside a 64-unit leaf.
    std::vector<NodeRange> cover(std::uint64_t l, std::uint64_t r,
                                 std::size_t k) const;

    // Lowest single node whose range contains [l, r).
    NodeRange lowest_cover(std::uint64_t l, std::uint64_t r) const;

private:
    struct Candidate {
        bool valid{false};
        std::uint64_t extra{0};
        std::vector<NodeRange> nodes;
    };
    Candidate solve(std::uint64_t index, std::uint64_t l, std::uint64_t r,
                    std::size_t k) const;
    Candidate solve_children(std::uint64_t index, std::uint64_t l,
                             std::uint64_t r, std::size_t k) const;

    int height_{0};
    std::uint64_t units_{0};
    std::vector<std::uint64_t> level_start_;
};

}  // namespace citron
