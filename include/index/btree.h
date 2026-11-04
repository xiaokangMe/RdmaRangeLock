#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "common/range.h"
#include "common/types.h"

namespace rrl {

enum class LatchMode : std::uint8_t {
    Shared,
    Exclusive,
};

// Actual leaf page identity and its routing fence (including empty gaps).
struct LeafSpan { std::uint64_t id{0}; KeyRangeEnt range; };

struct KeyValue {
    idx_key_t key{0};
    void* value{nullptr};
};

// Values are copied by a remote backend; pointers never cross address spaces.
struct IndexMutation {
    idx_key_t key{0};
    const std::uint64_t* value{nullptr};
    bool remove{false};
};

// Common offset representation for every RDMA protocol. The host BTreePage
// below contains C++ process pointers and cannot itself be registered as a
// portable remote page. Keys use the same unsigned ordering in both formats.
struct OffsetBTreeHeader {
    std::uint64_t latch{0}, poison{0};
    std::uint64_t magic{0}, format{2}, bytes{0}, order{0}, page_bytes{0};
    std::uint64_t page_capacity{0}, record_capacity{0}, record_base{0};
    std::uint64_t root{0}, pages_used{0}, records_used{0}, live_records{0}, height{1};
    std::uint64_t seed_fingerprint{0};
};
struct OffsetBTreePage {
    std::uint64_t leaf{1}, count{0}, next{0}, prev{0};
    // Followed by order keys and order+1 child/record offsets.
};
struct OffsetBTreeRecord {
    std::uint64_t key{0}, value{0}, valid{0}, latch{0};
};
static_assert(sizeof(OffsetBTreeHeader) == 128, "stable RDMA header");
static_assert(sizeof(OffsetBTreePage) == 32, "stable RDMA page prefix");
static_assert(sizeof(OffsetBTreeRecord) == 32, "stable RDMA record");

// Common page layout for v1 and v2. v1 leaves latch unused; v2 uses it for
// parent-to-child latch coupling.
struct BTreePage {
    std::uint64_t identity{0};
    bool is_leaf{true};
    std::vector<idx_key_t> keys;
    std::vector<void*> values;
    std::vector<std::unique_ptr<BTreePage>> children;
    BTreePage* parent{nullptr};
    BTreePage* prev{nullptr};
    BTreePage* next{nullptr};
    mutable std::shared_mutex latch;
};

class BTreeIndex {
public:
    virtual ~BTreeIndex() = default;
    virtual RC init(std::size_t order) = 0;
    virtual RC insert(idx_key_t key, void* value) = 0;
    virtual RC erase(idx_key_t key) = 0;
    virtual bool lookup(idx_key_t key, void*& value) const = 0;
    virtual std::vector<KeyValue> scan_range(const KeyRangeEnt& range) const = 0;
    virtual std::size_t size() const = 0;
    // Caller holds the lock-layer topology lease while using this snapshot.
    virtual bool leaf_cover_into(const KeyRangeEnt&,std::vector<LeafSpan>&) const { return false; }
    // Backend lifecycle hooks run before Begin/after End, outside txn locks.
    virtual bool preloaded() const { return false; }
    virtual void prepare_thread(std::size_t, std::size_t) {}
    virtual void finish_thread() noexcept {}
    virtual void prepare_write_capacity(std::size_t) {}
    virtual bool healthy() const { return true; }
    virtual void scan_range_into(const KeyRangeEnt& range, std::vector<KeyValue>& out) const {
        out = scan_range(range);
    }
    virtual RC apply_batch(const std::vector<IndexMutation>& changes) {
        for (const auto& change : changes) {
            const auto rc = change.remove ? erase(change.key) :
                insert(change.key, const_cast<std::uint64_t*>(change.value));
            if (rc != RCOK && !(change.remove && rc == Abort)) return rc;
        }
        return RCOK;
    }
};

class BTreeSingle final : public BTreeIndex {
public:
    BTreeSingle() = default;
    ~BTreeSingle() override = default;
    RC init(std::size_t order) override;
    RC insert(idx_key_t key, void* value) override;
    RC erase(idx_key_t key) override;
    bool lookup(idx_key_t key, void*& value) const override;
    std::vector<KeyValue> scan_range(const KeyRangeEnt& range) const override;
    std::size_t size() const override { return size_; }
    bool leaf_cover_into(const KeyRangeEnt&,std::vector<LeafSpan>&) const override;
    bool validate() const;
    std::size_t height() const;
    std::size_t order() const { return order_; }

private:
    using Page = BTreePage;
    std::unique_ptr<Page> make_page(bool leaf) const;
    Page* find_leaf(idx_key_t key) const;
    std::unique_ptr<Page> insert_recursive(Page& page, idx_key_t key, void* value, idx_key_t& separator);
    void split_leaf(Page& page, std::unique_ptr<Page>& right, idx_key_t& separator);
    void split_internal(Page& page, std::unique_ptr<Page>& right, idx_key_t& separator);
    bool refresh_separators(Page& page, idx_key_t& first_key);
    bool validate_page(const Page* page, const Page* expected_parent, idx_key_t* min_key,
                       idx_key_t* max_key, std::size_t* leaf_count) const;
    static bool range_ended(const KeyRangeEnt& range, idx_key_t key);
    mutable std::atomic<std::uint64_t> next_page_identity_{2};
    std::size_t order_{0};
    std::size_t size_{0};
    std::unique_ptr<Page> root_;
};

// v2: concurrent B+ tree using shared/exclusive page latches and crabbing.
// Writers are serialized only at the root-update boundary; ordinary readers
// use shared latch coupling and do not take the writer mutex.
class BTreeLatch final : public BTreeIndex {
public:
    BTreeLatch() = default;
    ~BTreeLatch() override = default;
    RC init(std::size_t order) override;
    RC insert(idx_key_t key, void* value) override;
    RC erase(idx_key_t key) override;
    bool lookup(idx_key_t key, void*& value) const override;
    std::vector<KeyValue> scan_range(const KeyRangeEnt& range) const override;
    std::size_t size() const override;
    bool leaf_cover_into(const KeyRangeEnt&,std::vector<LeafSpan>&) const override;
    bool validate() const;
    std::size_t height() const;
    std::size_t order() const { return order_; }

private:
    using Page = BTreePage;
    std::unique_ptr<Page> make_page(bool leaf) const;
    RC insert_new(idx_key_t key, void* value);
    RC insert_descent(idx_key_t key, void* value);
    RC insert_pessimistic(idx_key_t key, void* value);
    RC insert_forcing_root_split(idx_key_t key, void* value);
    std::unique_ptr<Page> insert_recursive(Page& page, idx_key_t key, void* value, idx_key_t& separator);
    void split_leaf(Page& page, std::unique_ptr<Page>& right, idx_key_t& separator);
    void split_internal(Page& page, std::unique_ptr<Page>& right, idx_key_t& separator);
    static bool range_ended(const KeyRangeEnt& range, idx_key_t key);
    static idx_key_t first_key(const Page& page);
    static std::size_t page_slot(const Page& parent, const Page* child);
    bool validate_page(const Page* page, const Page* expected_parent, std::size_t depth,
                       idx_key_t* min_key, idx_key_t* max_key, std::size_t* leaf_count,
                       std::size_t* leaf_depth) const;

    mutable std::atomic<std::uint64_t> next_page_identity_{2};
    std::size_t order_{0};
    // Writers increment under different leaf latches, so the count must be
    // atomic; readers only need a relaxed load.
    std::atomic<std::size_t> size_{0};
    std::unique_ptr<Page> root_;
    // Guards the root pointer only. Structural writes latch pages, and the
    // exclusive mode is used just for root creation, root split and root
    // collapse. There is no tree-wide writer mutex any more.
    mutable std::shared_mutex root_guard_;
};

}  // namespace rrl
