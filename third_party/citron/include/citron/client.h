// Citron lock acquisition / release (FAST'23 Alg.1-3) on a node layout that
// uses only standard 8-byte RDMA atomics. A client owns one QP (one thread)
// and holds no lock state: the remote tree is the only authority.
#pragma once

#include "citron/backend.h"
#include "citron/node_format.h"
#include "citron/segment_tree.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace citron {

struct CitronConfig {
    std::size_t k{2};                 // nodes per range (paper: 2)
    std::size_t m{4};                 // MITM distance (paper: 4)
    std::uint64_t t_wait_ns{15000};   // paper: 15 us
    std::uint64_t lock_deadline_ns{20000000};
    std::uint64_t leaf_starvation_ns{2000000};
    std::uint64_t leaf_retry_limit{100000};
    // No-wait integration: report a conflict as soon as it is observed instead
    // of spinning until the deadline (the txn driver retries the transaction).
    bool abort_on_conflict{false};
};

struct HeldNode {
    SegmentTree::NodeRange node;
    bool writer{true};
    bool leaf_locked{false};
    bool occupied{false};
    bool ticketed{false};
    std::uint64_t leaf_mask{0};
    std::vector<std::uint64_t> notified;
};

struct RangeLock {
    std::uint64_t lo{0};          // requested range
    std::uint64_t hi{0};
    std::uint64_t covered_lo{0};  // physical span actually locked
    std::uint64_t covered_hi{0};
    bool spillover{false};
    std::vector<HeldNode> nodes;
};

class CitronClient {
public:
    enum class Status { Acquired, Conflict, Aborted, Error, OverCover };

    CitronClient(std::unique_ptr<Backend> backend, SegmentTree tree,
                 CitronConfig config);

    // writer=true is the paper's exclusive range lock; writer=false is the
    // shared-reader extension used when the workload takes S locks.
    Status acquire(std::uint64_t lo, std::uint64_t hi, bool writer,
                   RangeLock& out);
    bool release(RangeLock& lock);
    bool release_tree_nodes(RangeLock& lock);
    bool release_partial(RangeLock& lock);

    const SegmentTree& tree() const { return tree_; }
    const CitronConfig& config() const { return config_; }
    Backend& backend() { return *backend_; }
    std::uint64_t operations() const;

    struct Stats {
        std::uint64_t lock_calls{0};
        std::uint64_t acquired{0};
        std::uint64_t conflicts{0};
        std::uint64_t aborts{0};
        std::uint64_t over_cover{0};
        std::uint64_t leaf_retries{0};
        std::uint64_t ancestor_waits{0};
        std::uint64_t descendant_waits{0};
        std::uint64_t notification_rounds{0};
    };
    const Stats& stats() const { return stats_; }

private:
    bool release_node(HeldNode& held);
    bool poisoned();
    void poison() noexcept;
    std::vector<std::uint64_t> notification_targets(std::uint64_t index) const;

    std::unique_ptr<Backend> backend_;
    SegmentTree tree_;
    CitronConfig config_;
    Stats stats_;
};

}  // namespace citron
