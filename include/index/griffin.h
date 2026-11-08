#pragma once
#include "index/btree.h"
#include <stdexcept>

namespace rrl {
// Transient structural staleness, not a data/transport fault. The txn driver
// aborts this attempt rather than returning an incomplete range result.
struct IndexSyncPending : std::runtime_error {
    IndexSyncPending():std::runtime_error("Griffin committed key not synchronized"){}
};

// Griffin §III/IV: hash is authoritative; the ordered index stores only keys.
// This class contains no transaction IDs, lock grants or wait-policy state.
// Transaction-level Lp/Lu and record locking remain in src/txn and src/lock.
class BTreeGriffin final : public BTreeIndex {
public:
    explicit BTreeGriffin(std::size_t queue_capacity=8192);
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
private:
    friend struct BTreeGriffinTestPeer;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rrl
