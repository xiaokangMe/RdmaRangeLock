#pragma once

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

struct KeyValue {
    idx_key_t key{0};
    void* value{nullptr};
};

// Common page layout for v1 and v2. v1 leaves latch unused; v2 uses it for
// parent-to-child latch coupling.
struct BTreePage {
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
    bool validate() const;
    std::size_t height() const;
    std::size_t order() const { return order_; }

private:
    using Page = BTreePage;
    std::unique_ptr<Page> make_page(bool leaf) const;
    bool update_existing(idx_key_t key, void* value);
    std::unique_ptr<Page> insert_recursive(Page& page, idx_key_t key, void* value, idx_key_t& separator);
    void split_leaf(Page& page, std::unique_ptr<Page>& right, idx_key_t& separator);
    void split_internal(Page& page, std::unique_ptr<Page>& right, idx_key_t& separator);
    static bool range_ended(const KeyRangeEnt& range, idx_key_t key);
    static idx_key_t first_key(const Page& page);
    bool validate_page(const Page* page, const Page* expected_parent, idx_key_t* min_key,
                       idx_key_t* max_key, std::size_t* leaf_count) const;

    std::size_t order_{0};
    std::size_t size_{0};
    std::unique_ptr<Page> root_;
    mutable std::shared_mutex root_guard_;
    mutable std::mutex writer_guard_;
};

}  // namespace rrl