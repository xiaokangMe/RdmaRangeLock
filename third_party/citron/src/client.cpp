#include "citron/client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <stdexcept>

namespace citron {
namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point deadline_after(std::uint64_t ns) {
    return Clock::now() + std::chrono::nanoseconds(ns);
}

bool expired(Clock::time_point deadline) { return Clock::now() >= deadline; }

constexpr std::uint64_t kDecrement = ~0ull;

}  // namespace

CitronClient::CitronClient(std::unique_ptr<Backend> backend, SegmentTree tree,
                           CitronConfig config)
    : backend_(std::move(backend)), tree_(std::move(tree)), config_(config) {
    if(!backend_ || !config_.k || config_.k>2 || !config_.m || config_.m>16 ||
       !config_.t_wait_ns || config_.t_wait_ns>1000000000ULL ||
       config_.lock_deadline_ns<config_.t_wait_ns)
        throw std::invalid_argument("invalid Citron geometry or timing bounds");
}

std::uint64_t CitronClient::operations() const { return backend_->operations(); }

std::vector<std::uint64_t> CitronClient::notification_targets(
    std::uint64_t index) const {
    std::vector<std::uint64_t> targets;
    if (index <= 1) return targets;  // root has no ancestors
    const int depth = tree_.depth_of(index);
    const int m = static_cast<int>(config_.m);
    if (depth <= m) {
        for (std::uint64_t cur = SegmentTree::parent(index); cur != 0;
             cur = SegmentTree::parent(cur)) {
            targets.push_back(cur);
            if (cur == 1) break;
        }
        return targets;
    }

    targets.push_back(SegmentTree::parent(index));
    std::uint64_t cur = SegmentTree::parent(index);
    int cur_depth = depth - 1;
    for (;;) {
        for (int i = 0; i < m && cur > 1; ++i) {
            cur = SegmentTree::parent(cur);
            --cur_depth;
        }
        if (cur == 1) break;
        if (cur_depth < m) {
            // Top m levels are replaced by the level-m ancestor (paper §3.5.5);
            // the root is notified too so a root-level locker sees it.
            std::uint64_t replacement = index;
            for (int i = 0; i < depth - m; ++i)
                replacement = SegmentTree::parent(replacement);
            if (std::find(targets.begin(), targets.end(), replacement) == targets.end())
                targets.push_back(replacement);
            if (std::find(targets.begin(), targets.end(), 1ull) == targets.end())
                targets.push_back(1);
            break;
        }
        targets.push_back(cur);
    }
    return targets;
}

CitronClient::Status CitronClient::acquire(std::uint64_t lo, std::uint64_t hi,
                                           bool writer, RangeLock& out) {
    ++stats_.lock_calls;
    out = RangeLock{};
    if (!writer || hi <= lo || hi > tree_.units()) return Status::Error;
    if (poisoned()) return Status::Error;
    try {
    out.lo = lo;
    out.hi = hi;

    const auto deadline = deadline_after(config_.lock_deadline_ns);
    const std::uint64_t units = tree_.units();
    const std::uint64_t spill_index = tree_.total_nodes() + 1;

    if (hi > units) {
        // Out-of-bound part: spillover mutex (Alg.1).
        std::uint64_t previous = 0;
        if (!backend_->faa(spill_index, kTMax, 1, previous)) return Status::Error;
        const std::uint32_t ticket = static_cast<std::uint32_t>(previous);
        for (;;) {
            Node node;
            if (!backend_->read_node(spill_index, node)) return Status::Error;
            if (node_tcnt(node) == ticket) break;
            // A ticket may not be skipped: advancing TCnt out of order would
            // let a later ticket enter while the current holder is inside.
            std::this_thread::yield();
        }
        out.spillover = true;
    }

    const std::uint64_t tree_hi = std::min(hi, units);
    for (;;) {
        std::vector<SegmentTree::NodeRange> cover;
        if (lo < tree_hi) cover = tree_.cover(lo, tree_hi, config_.k);
        out.nodes.clear();
        out.nodes.reserve(cover.size());
        for (const auto& range_node : cover) {
            out.nodes.emplace_back();
            auto& held=out.nodes.back();
            // Set before any verb so a failed attempt can undo exactly the
            // node it may have touched (ticket/Occ/leaf bits/notifications).
            held.node = range_node;
            held.writer = writer;
            const Status status = [&] {
                const bool leaf = range_node.leaf;
                std::uint64_t ticket = 0;
                if (!leaf) {
                    // No-wait admission atomically claims the next ticket
                    // only while the queue is empty. Never abandon a queued
                    // FAA ticket, which would block all successors forever.
                    std::uint64_t previous=0;
                    if(config_.abort_on_conflict) {
                        Node node;
                        if(!backend_->read_node(range_node.index,node))return Status::Error;
                        const auto next=node_tmax(node);
                        if(next!=node_tcnt(node))return Status::Conflict;
                        if(!backend_->cas(range_node.index,kTMax,next,next+1,previous))return Status::Error;
                        if(previous!=next)return Status::Conflict;
                        held.ticketed=true;
                    } else {
                        if(!backend_->faa(range_node.index,kTMax,1,previous))return Status::Error;
                        ticket=previous;
                        for(;;) {
                            Node node;
                            if(!backend_->read_node(range_node.index,node))return Status::Error;
                            if(node_tcnt(node)==ticket)break;
                            if(poisoned())return Status::Error;
                            std::this_thread::yield();
                        }
                        held.ticketed=true;
                    }
                }

                // 2(b): wait until every conflicting ancestor drained.
                // t1 is the post time of the last ancestor read; the paper's
                // Eq.1 uses it to bound how long a notification may take.
                Clock::time_point t1 = Clock::now();
                std::uint64_t cleared = range_node.index;
                while (cleared != 1) {
                    const auto chain = tree_.ancestors(cleared);
                    std::vector<Node> nodes;
                    // Eq.1 uses the *post* time of the last ancestor read, so
                    // that t1 < t0 is guaranteed when the read missed Alice's
                    // Occ write.
                    t1 = Clock::now();
                    if (!backend_->read_node_many(chain, nodes))
                        return Status::Error;
                    if (chain.empty()) break;
                    if (node_exp(nodes.back())) return Status::Aborted;
                    int chosen = -1;
                    for (std::size_t i = 0; i < nodes.size(); ++i) {
                        const bool busy = writer
                                              ? (node_occw(nodes[i]) || node_occr(nodes[i]))
                                              : node_occw(nodes[i]);
                        if (busy) {
                            chosen = static_cast<int>(i);
                            break;
                        }
                    }
                    if (chosen < 0) break;
                    if (config_.abort_on_conflict) {
                        ++stats_.conflicts;
                        return Status::Conflict;
                    }
                    const std::uint64_t next = chain[static_cast<std::size_t>(chosen)];
                    ++stats_.ancestor_waits;
                    for (;;) {
                        Node node;
                        if (!backend_->read_node(next, node)) return Status::Error;
                        const bool busy = writer
                                              ? (node_occw(node) || node_occr(node))
                                              : node_occw(node);
                        if (!busy) break;
                        if (expired(deadline)) return Status::Aborted;
                        std::this_thread::yield();
                    }
                    cleared = next;
                }

                // 2(c): occupy the node, or set the leaf bitmap.
                if (leaf) {
                    const std::uint64_t mask =
                        leaf_mask(lo, tree_hi, range_node.lo, range_node.hi);
                    if (!mask) return Status::Acquired;
                    const auto started = Clock::now();
                    for (;;) {
                        Node node;
                        if (!backend_->read_node(range_node.index, node))
                            return Status::Error;
                        const std::uint64_t bitmap_w = node.words[kBitmapW];
                        const std::uint64_t bitmap_r = node.words[kBitmapR];
                        const std::uint64_t busy =
                            writer ? (bitmap_w | bitmap_r) : bitmap_w;
                        if (busy & mask) {
                            ++stats_.leaf_retries;
                            if (config_.abort_on_conflict) {
                                ++stats_.conflicts;
                                return Status::Conflict;
                            }
                            if (std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    Clock::now() - started)
                                        .count() >
                                    static_cast<long long>(config_.leaf_starvation_ns) ||
                                stats_.leaf_retries > config_.leaf_retry_limit) {
                                // The paper switches to the parent node here.
                                // We keep the exact cover instead: the parent
                                // path is only needed for starvation, and our
                                // leaf update is a lock-free CAS loop.
                                if (expired(deadline)) {
                                    ++stats_.aborts;
                                    return Status::Aborted;
                                }
                            }
                            const auto chain = tree_.ancestors(range_node.index);
                            std::vector<Node> nodes;
                            if (!backend_->read_node_many(chain, nodes))
                                return Status::Error;
                            if (!nodes.empty() && node_exp(nodes.back()))
                                return Status::Aborted;
                            if (expired(deadline)) return Status::Aborted;
                            std::this_thread::yield();
                            continue;
                        }
                        const int field = writer ? kBitmapW : kBitmapR;
                        const std::uint64_t current =
                            writer ? bitmap_w : bitmap_r;
                        std::uint64_t previous = 0;
                        if (!backend_->cas(range_node.index, field, current,
                                           current | mask, previous))
                            return Status::Error;
                        if (previous == current) {
                            held.leaf_mask = mask;
                            held.leaf_locked = true;
                            break;
                        }
                        if (expired(deadline)) return Status::Aborted;
                    }
                } else {
                    std::uint64_t previous = 0;
                    if (!backend_->faa(range_node.index,
                                       writer ? kOccW : kOccR, 1, previous))
                        return Status::Error;
                    held.occupied = true;
                }

                // 2(d): notify ancestors, then wait for conflicting descendants.
                const auto t0 = Clock::now();
                held.writer = writer;
                const auto targets = notification_targets(range_node.index);
                held.notified.reserve(targets.size());
                if (!targets.empty()) {
                    for(const auto target:targets) {
                        std::uint64_t previous=0;
                        if(!backend_->faa(target,writer?kDMaxW:kDMaxR,1,previous))return Status::Error;
                        held.notified.push_back(target);
                    }
                    // FAST'23 Eq.1: a notifier must finish within
                    // (1-delta)*Twait of its last ancestor read, otherwise an
                    // ancestor may already have stopped waiting.
                    const auto t2 = Clock::now();
                    const auto budget = std::chrono::nanoseconds(
                        static_cast<std::uint64_t>(
                            config_.t_wait_ns * 0.9999));
                    if (range_node.index != 1 && t2 - t1 > budget)
                        return Status::Aborted;
                    ++stats_.notification_rounds;
                }
                Node root;
                if (!backend_->read_node(1, root)) return Status::Error;
                if (node_exp(root)) return Status::Aborted;
                if (!held.notified.empty() && held.notified.back() != 1) {
                    Node high;
                    if (!backend_->read_node(held.notified.back(), high))
                        return Status::Error;
                    if (node_exp(high)) return Status::Aborted;
                }

                if (!leaf) {
                    const auto wake =
                        t0 + std::chrono::nanoseconds(config_.t_wait_ns);
                    while (Clock::now() < wake) {
                        if (expired(deadline)) return Status::Aborted;
                        std::this_thread::yield();
                    }
                    ++stats_.descendant_waits;
                    const int depth = tree_.depth_of(range_node.index);
                    const int last =
                        std::min(depth + static_cast<int>(config_.m) - 1,
                                 tree_.height() - 1);
                    std::uint64_t first = range_node.index;
                    for (int level = depth; level <= last; ++level) {
                        const std::size_t count = 1ull << (2 * (level - depth));
                        std::vector<Node> nodes;
                        if (!backend_->read_node_range(first, count, nodes))
                            return Status::Error;
                        for (std::size_t i = 0; i < nodes.size(); ++i) {
                            const bool settled =
                                node_dcnt(nodes[i], true) == node_dmax(nodes[i], true) &&
                                (!writer || (node_dcnt(nodes[i], false) ==
                                             node_dmax(nodes[i], false)));
                            if (settled) continue;
                            if(config_.abort_on_conflict)return Status::Conflict;
                            const std::uint64_t target = first + i;
                            for (;;) {
                                Node node;
                                if (!backend_->read_node(target, node))
                                    return Status::Error;
                                const bool ok =
                                    node_dcnt(node, true) == node_dmax(node, true) &&
                                    (!writer || (node_dcnt(node, false) ==
                                                 node_dmax(node, false)));
                                if (ok) break;
                                if (expired(deadline)) return Status::Aborted;
                                std::this_thread::yield();
                            }
                        }
                        if (level < last) first = SegmentTree::child(first, 0);
                    }
                }

                return Status::Acquired;
            }();

            if (status == Status::Acquired) {
                continue;
            }
            if(status==Status::Error) {poison();return Status::Error;}
            if(!release_partial(out)){poison();return Status::Error;}
            out = RangeLock{};
            if (status == Status::Aborted) ++stats_.aborts;
            return status;
        }
        out.covered_lo = out.lo;
        out.covered_hi = std::min(out.hi, tree_.units());
        for (const auto& held_node : out.nodes) {
            if (held_node.node.leaf) continue;
            out.covered_lo = std::min(out.covered_lo, held_node.node.lo);
            out.covered_hi = std::max(out.covered_hi, held_node.node.hi);
        }
        if (out.spillover) out.covered_hi = ~0ull;
        ++stats_.acquired;
        return Status::Acquired;
    }
    }catch(...){poison();return Status::Error;}
}

bool CitronClient::poisoned() {
    Node node;
    return !backend_->read_node(tree_.total_nodes()+2,node) || node.words[kOccW]!=0;
}
void CitronClient::poison() noexcept {
    try {std::uint64_t previous=0;backend_->cas(tree_.total_nodes()+2,kOccW,0,1,previous);}catch(...){}
}
bool CitronClient::release_node(HeldNode& held) {
    std::uint64_t previous=0;
    if(held.leaf_locked) {
        for(;;) {
            Node node;if(!backend_->read_node(held.node.index,node))return false;
            const auto bitmap=node.words[kBitmapW];
            if((bitmap&held.leaf_mask)!=held.leaf_mask)return false;
            if(!backend_->cas(held.node.index,kBitmapW,bitmap,bitmap&~held.leaf_mask,previous))return false;
            if(previous==bitmap){held.leaf_locked=false;break;}
        }
    }
    if(held.occupied) {
        if(!backend_->faa(held.node.index,kOccW,kDecrement,previous))return false;
        held.occupied=false;
    }
    if(held.ticketed) {
        if(!backend_->faa(held.node.index,kTCnt,1,previous))return false;
        held.ticketed=false;
    }
    while(!held.notified.empty()) {
        if(!backend_->faa(held.notified.back(),kDCntW,1,previous))return false;
        held.notified.pop_back();
    }
    return true;
}
bool CitronClient::release(RangeLock& lock) {
    if(poisoned())return false;
    if(!release_tree_nodes(lock)){poison();return false;}
    if(lock.spillover){poison();return false;}
    return true;
}
bool CitronClient::release_tree_nodes(RangeLock& lock) {
    while(!lock.nodes.empty()) {
        if(!release_node(lock.nodes.back()))return false;
        lock.nodes.pop_back();
    }
    return true;
}
bool CitronClient::release_partial(RangeLock& lock){return release(lock);}
} // namespace citron
