#pragma once
#include "index/btree.h"
#include <stdexcept>
#include "common/latency_histogram.h"

namespace rrl {
// Transient structural staleness, not a data/transport fault. The txn driver
// aborts this attempt rather than returning an incomplete range result.
struct IndexSyncPending : std::runtime_error {
    IndexSyncPending():std::runtime_error("Griffin committed key not synchronized"){}
};

struct GriffinDiagnostics {
    std::uint64_t queue_batches{0},queue_peak_batches{0},pending_keys{0},pending_peak_keys{0};
    std::uint64_t committed_batches{0},membership_batches{0},membership_operations{0},value_only_batches{0};
    std::uint64_t capacity_aborts{0},sync_cycles{0},synced_batches{0},tree_operations{0};
    LatencySummary sync_work,sync_lag;
};

// Griffin §III/IV: hash is authoritative; the ordered index stores only keys.
// This class contains no transaction IDs, lock grants or wait-policy state.
// Transaction-level Lp/Lu and record locking remain in src/txn and src/lock.
class BTreeGriffin final : public BTreeIndex {
public:
    explicit BTreeGriffin(std::size_t queue_capacity=8192,std::size_t sync_batch_limit=64);
    ~BTreeGriffin() override;
    RC init(std::size_t) override;
    RC insert(idx_key_t,void*) override;
    RC erase(idx_key_t) override;
    RC apply_batch(const std::vector<IndexMutation>&) override;
    bool lookup(idx_key_t,void*&) const override;
    std::vector<KeyValue> scan_range(const KeyRangeEnt&) const override;
    void scan_range_into(const KeyRangeEnt&,std::vector<KeyValue>&) const override;
    std::size_t size() const override;
    void prepare_thread(std::size_t,std::size_t) override;
    bool healthy() const override;
    bool validate() const;
    void wait_synced() const;
    GriffinDiagnostics diagnostics() const;
    // Caller must quiesce transaction workers before resetting the window.
    void reset_diagnostics();
private:
    friend struct BTreeGriffinTestPeer;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rrl
