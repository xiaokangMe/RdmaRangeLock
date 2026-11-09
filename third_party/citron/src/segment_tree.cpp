#include "citron/segment_tree.h"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace citron {

SegmentTree::SegmentTree(int height) : height_(height) {
    if (height < 1 || height > 15)
        throw std::invalid_argument("segment tree height must be 1..15");
    // units = 64 * 4^height
    units_ = 64ull << (2 * height_);
    level_start_.resize(static_cast<std::size_t>(height_) + 2, 0);
    std::uint64_t nodes = 1;
    std::uint64_t power = 1;
    for (int d = 0; d <= height_ + 1; ++d) {
        level_start_[static_cast<std::size_t>(d)] = nodes;
        nodes += power;       // level d has 4^d nodes
        power *= 4;
    }
}

std::uint64_t SegmentTree::level_count(int depth) const {
    return 1ull << (2 * depth);
}

int SegmentTree::depth_of(std::uint64_t index) const {
    for (int d = 0; d <= height_; ++d)
        if (index >= level_start_[static_cast<std::size_t>(d)] &&
            index < level_start_[static_cast<std::size_t>(d) + 1])
            return d;
    throw std::out_of_range("segment tree index out of range");
}

SegmentTree::NodeRange SegmentTree::node(std::uint64_t index) const {
    const int d = depth_of(index);
    const std::uint64_t pos = index - level_start_[static_cast<std::size_t>(d)];
    const std::uint64_t width = units_ >> (2 * d);
    NodeRange r;
    r.index = index;
    r.lo = pos * width;
    r.hi = r.lo + width;
    r.leaf = (d == height_);
    return r;
}

std::vector<std::uint64_t> SegmentTree::ancestors(std::uint64_t index) const {
    std::vector<std::uint64_t> out;
    for (std::uint64_t x = index; x > 1;) {
        x = parent(x);
        out.push_back(x);
    }
    return out;
}

SegmentTree::Candidate SegmentTree::solve_children(std::uint64_t index,
                                                   std::uint64_t l,
                                                   std::uint64_t r,
                                                   std::size_t k) const {
    Candidate best;
    Candidate acc;
    acc.valid = true;

    std::function<void(int, std::size_t, Candidate&)> rec =
        [&](int ci, std::size_t budget, Candidate& cur) {
            if (ci == 4) {
                if (cur.valid && (!best.valid || cur.extra < best.extra ||
                                  (cur.extra == best.extra &&
                                   cur.nodes.size() < best.nodes.size()))) {
                    best = cur;
                }
                return;
            }
            const std::uint64_t ch = child(index, ci);
            const NodeRange cr = node(ch);
            const bool intersects = cr.lo < r && l < cr.hi;
            if (!intersects) {
                rec(ci + 1, budget, cur);
                return;
            }
            for (std::size_t take = 1; take <= budget; ++take) {
                Candidate sub = solve(ch, l, r, take);
                if (!sub.valid) continue;
                Candidate next = cur;
                next.valid = true;
                next.extra += sub.extra;
                next.nodes.insert(next.nodes.end(), sub.nodes.begin(),
                                  sub.nodes.end());
                rec(ci + 1, budget - take, next);
            }
        };
    rec(0, k, acc);
    return best;
}

SegmentTree::Candidate SegmentTree::solve(std::uint64_t index,
                                          std::uint64_t l, std::uint64_t r,
                                          std::size_t k) const {
    Candidate result;
    if (k == 0) return result;
    const NodeRange nr = node(index);
    if (nr.hi <= l || r <= nr.lo) return result;

    // Case 1: the node lies completely inside the requested range, so locking
    // it is exact and uses a single node. This dominates any split.
    if (l <= nr.lo && nr.hi <= r) {
        result.valid = true;
        result.extra = 0;
        result.nodes.push_back(nr);
        return result;
    }

    // A leaf can mask any sub-range with its bitmap.
    if (nr.leaf) {
        result.valid = true;
        result.extra = 0;
        result.nodes.push_back(nr);
        return result;
    }

    // Case 2: the requested range is contained in this node. The node itself
    // can be locked with over-coverage, or the request can be split among the
    // children (knapsack).
    if (nr.lo <= l && r <= nr.hi) {
        result.valid = true;
        result.extra = (nr.hi - nr.lo) - (r - l);
        result.nodes.push_back(nr);
        if (k >= 1) {
            Candidate kids = solve_children(index, l, r, k);
            if (kids.valid && (kids.extra < result.extra ||
                               (kids.extra == result.extra &&
                                kids.nodes.size() < result.nodes.size()))) {
                result = kids;
            }
        }
        return result;
    }

    // Case 3: partial overlap; children must cover their own intersections.
    return solve_children(index, l, r, k);
}

std::vector<SegmentTree::NodeRange> SegmentTree::cover(std::uint64_t l,
                                                       std::uint64_t r,
                                                       std::size_t k) const {
    std::vector<NodeRange> out;
    if (l >= r || k == 0) return out;
    if (r > units_) r = units_;
    if (l >= units_) return out;
    Candidate best = solve(1, l, r, k);
    if (!best.valid) return out;
    out = std::move(best.nodes);
    std::sort(out.begin(), out.end(),
              [](const NodeRange& a, const NodeRange& b) { return a.lo < b.lo; });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const NodeRange& a, const NodeRange& b) {
                              return a.index == b.index;
                          }),
              out.end());
    return out;
}

SegmentTree::NodeRange SegmentTree::lowest_cover(std::uint64_t l,
                                                 std::uint64_t r) const {
    if (r > units_) r = units_;
    std::uint64_t cur = 1;
    for (;;) {
        bool descended = false;
        for (int i = 0; i < 4; ++i) {
            const std::uint64_t ch = child(cur, i);
            if (ch > total_nodes()) break;
            const NodeRange cr = node(ch);
            if (cr.lo <= l && r <= cr.hi) {
                cur = ch;
                descended = true;
                break;
            }
        }
        if (!descended) break;
    }
    return node(cur);
}

}  // namespace citron
