#pragma once
#include "index/btree.h"
#include <string>

namespace rrl {
struct RdmaTreeConfig {
    std::size_t order{128}, page_capacity{8192}, record_capacity{262144};
    std::uint64_t latch_timeout_ms{10000};
};
using TreeSeed = std::vector<std::pair<idx_key_t, std::uint64_t>>;
std::uint64_t tree_seed_fingerprint(const TreeSeed&);

// Initialized before registration; thereafter the server CPU never touches it.
class RdmaTreeMemory {
public:
    RdmaTreeMemory(const RdmaTreeConfig&, const TreeSeed&);
    ~RdmaTreeMemory();
    void* data();
    std::size_t bytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One-sided remote index AND records, shared by all clients and protocols.
class BTreeRdma final : public BTreeIndex {
public:
    BTreeRdma();
    ~BTreeRdma() override;
    bool connect(const std::string&, std::uint16_t, std::size_t workers,
                 std::uint64_t expected_seed, std::uint64_t latch_timeout_ms = 10000);
    RC init(std::size_t order) override;
    RC insert(idx_key_t, void*) override;
    RC erase(idx_key_t) override;
    bool lookup(idx_key_t, void*&) const override;
    std::vector<KeyValue> scan_range(const KeyRangeEnt&) const override;
    void scan_range_into(const KeyRangeEnt&, std::vector<KeyValue>&) const override;
    std::size_t size() const override;
    bool preloaded() const override { return true; }
    void prepare_thread(std::size_t, std::size_t scan_capacity) override;
    void finish_thread() noexcept override;
    void prepare_write_capacity(std::size_t) override;
    bool healthy() const override;
    RC apply_batch(const std::vector<IndexMutation>&) override;
    bool key_neighbors(idx_key_t, IndexKeyNeighbors&) const override;
    bool key_range_into(const KeyRangeEnt&, std::vector<idx_key_t>&) const override;
    bool leaf_cover_into(const KeyRangeEnt&,std::vector<LeafSpan>&) const override;
    bool hierarchy_cover_into(const KeyRangeEnt&,std::vector<IndexNodeSpan>&) const override;
    bool validate() const;
    std::uint64_t rdma_operations() const;
    std::uint64_t read_bytes() const;
    std::uint64_t write_bytes() const;
    std::uint64_t latch_wait_ns() const;
    std::size_t record_capacity() const;
private:
    friend struct BTreeRdmaTestPeer;
    void (*before_value_store_test_hook_)() = nullptr;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rrl
