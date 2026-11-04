#include "leaf_cover.h"
#include "index/btree.h"

#include <algorithm>
#include <shared_mutex>

namespace rrl {

namespace {

constexpr std::size_t kMinOrder = 3;

// A node is safe to insert into when one more key still leaves it below the
// overflow threshold, so the insert cannot propagate a split to its parent.
bool inserts_without_split(std::size_t keys, std::size_t order) {
    return keys + 1 < order;
}

}  // namespace

RC BTreeLatch::init(std::size_t order) {
    if (order < kMinOrder) return ERROR;
    std::unique_lock<std::shared_mutex> root_lock(root_guard_);
    order_ = order;
    root_.reset();
    size_.store(0, std::memory_order_relaxed);
    return RCOK;
}

std::unique_ptr<BTreeLatch::Page> BTreeLatch::make_page(bool leaf) const {
    auto page = std::make_unique<Page>();
    page->identity=next_page_identity_.fetch_add(1);
    page->is_leaf = leaf;
    page->keys.reserve(order_);
    if (leaf) page->values.reserve(order_);
    else page->children.reserve(order_ + 1);
    return page;
}

void BTreeLatch::split_leaf(Page& page, std::unique_ptr<Page>& right,
                            idx_key_t& separator) {
    right = make_page(true);
    right->parent = page.parent;
    const std::size_t split = page.keys.size() / 2;
    right->keys.assign(page.keys.begin() + static_cast<std::ptrdiff_t>(split), page.keys.end());
    right->values.assign(page.values.begin() + static_cast<std::ptrdiff_t>(split), page.values.end());
    page.keys.resize(split);
    page.values.resize(split);
    // The right neighbour's prev pointer is part of the leaf chain, so it must
    // be latched before it is rewritten. Splitters always latch left-to-right,
    // so two concurrent splits can never deadlock on each other's sibling.
    right->prev = &page;
    right->next = page.next;
    if (page.next != nullptr) {
        std::unique_lock<std::shared_mutex> next_lock(page.next->latch);
        page.next->prev = right.get();
    }
    page.next = right.get();
    separator = right->keys.front();
}

void BTreeLatch::split_internal(Page& page, std::unique_ptr<Page>& right,
                                idx_key_t& separator) {
    right = make_page(false);
    right->parent = page.parent;
    const std::size_t middle = page.keys.size() / 2;
    separator = page.keys[middle];
    right->keys.assign(page.keys.begin() + static_cast<std::ptrdiff_t>(middle + 1), page.keys.end());
    for (std::size_t i = middle + 1; i < page.children.size(); ++i) {
        right->children.push_back(std::move(page.children[i]));
        right->children.back()->parent = right.get();
    }
    page.keys.resize(middle);
    page.children.resize(middle + 1);
}

// Only reached with the tree-wide guard held exclusively, so a plain
// recursive descent is safe and no concurrent split can be observed.
std::unique_ptr<BTreeLatch::Page> BTreeLatch::insert_recursive(
    Page& page, idx_key_t key, void* value, idx_key_t& separator) {
    std::unique_lock<std::shared_mutex> page_lock(page.latch);
    if (page.is_leaf) {
        const auto pos = std::lower_bound(page.keys.begin(), page.keys.end(), key);
        const std::size_t index = static_cast<std::size_t>(pos - page.keys.begin());
        if (pos != page.keys.end() && *pos == key) {
            page.values[index] = value;
            return nullptr;
        }
        page.keys.insert(pos, key);
        page.values.insert(page.values.begin() + static_cast<std::ptrdiff_t>(index), value);
        size_.fetch_add(1, std::memory_order_relaxed);
        if (page.keys.size() < order_) return nullptr;
        std::unique_ptr<Page> right;
        split_leaf(page, right, separator);
        return right;
    }

    const auto pos = std::upper_bound(page.keys.begin(), page.keys.end(), key);
    const std::size_t child_index = static_cast<std::size_t>(pos - page.keys.begin());
    idx_key_t child_separator = 0;
    auto right_child = insert_recursive(*page.children[child_index], key, value, child_separator);
    if (right_child == nullptr) return nullptr;
    page.keys.insert(page.keys.begin() + static_cast<std::ptrdiff_t>(child_index), child_separator);
    page.children.insert(page.children.begin() + static_cast<std::ptrdiff_t>(child_index + 1), std::move(right_child));
    page.children[child_index + 1]->parent = &page;
    if (page.keys.size() < order_) return nullptr;
    std::unique_ptr<Page> right;
    split_internal(page, right, separator);
    return right;
}

RC BTreeLatch::insert(idx_key_t key, void* value) {
    if (order_ == 0) return ERROR;
    return insert_new(key, value);
}

// Inserts a key that is known not to be in the tree. One shared-latch descent
// plus an exclusive leaf latch covers the common case; a full leaf falls
// through to the crabbing split path.
RC BTreeLatch::insert_new(idx_key_t key, void* value) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::shared_lock<std::shared_mutex> root_shared(root_guard_);
        if (root_ == nullptr) break;
        // Two-latch crabbing: keep only the parent and the node being visited.
        // Holding every ancestor while upgrading the leaf made concurrent
        // updates contend on shared latches they never needed.
        Page* page = root_.get();
        std::shared_lock<std::shared_mutex> current(page->latch);
        std::shared_lock<std::shared_mutex> previous;
        while (!page->is_leaf) {
            const auto pos = std::upper_bound(page->keys.begin(), page->keys.end(), key);
            Page* child = page->children[static_cast<std::size_t>(pos - page->keys.begin())].get();
            std::shared_lock<std::shared_mutex> child_lock(child->latch);
            previous = std::move(current);
            current = std::move(child_lock);
            page = child;
        }
        current.unlock();
        // A single-level tree has no parent latch to drop.
        if (previous.owns_lock()) previous.unlock();
        std::unique_lock<std::shared_mutex> leaf_lock(page->latch);
        // The parent was released before the upgrade, so a concurrent split may
        // have moved this key to a new right sibling. That shows up as the key
        // being above everything the leaf still holds, and the descent simply
        // restarts; holding the parent across the upgrade instead made every
        // update contend on the parent latch for no benefit.
        if (!page->keys.empty() && key > page->keys.back()) continue;
        const auto pos = std::lower_bound(page->keys.begin(), page->keys.end(), key);
        const std::size_t index = static_cast<std::size_t>(pos - page->keys.begin());
        if (pos != page->keys.end() && *pos == key) {
            page->values[index] = value;
            return RCOK;
        }
        if (inserts_without_split(page->keys.size(), order_)) {
            page->keys.insert(pos, key);
            page->values.insert(page->values.begin() + static_cast<std::ptrdiff_t>(index), value);
            size_.fetch_add(1, std::memory_order_relaxed);
            return RCOK;
        }
        break;  // leaf is full: fall through to the crabbing split path
    }
    return insert_pessimistic(key, value);
}

RC BTreeLatch::insert_descent(idx_key_t key, void* value) {
    std::shared_lock<std::shared_mutex> root_shared(root_guard_);
    if (root_ == nullptr) return WAIT;

    std::vector<Page*> path;
    std::vector<std::unique_lock<std::shared_mutex>> locks;
    path.reserve(8);
    locks.reserve(8);

    Page* page = root_.get();
    locks.emplace_back(page->latch);
    // Decide under the root latch, not before taking it: another inserter can
    // fill the root between the guard probe and this latch, and a root split
    // needs the guard exclusively. Returning before any modification keeps the
    // retry in insert_pessimistic clean.
    if (page->keys.size() >= order_ - 1) return WAIT;
    path.push_back(page);
    while (!page->is_leaf) {
        const auto pos = std::upper_bound(page->keys.begin(), page->keys.end(), key);
        Page* child = page->children[static_cast<std::size_t>(pos - page->keys.begin())].get();
        locks.emplace_back(child->latch);
        path.push_back(child);
        if (inserts_without_split(child->keys.size(), order_)) {
            locks.erase(locks.begin(), locks.end() - 1);
            path.erase(path.begin(), path.end() - 1);
        }
        page = child;
    }

    const auto pos = std::lower_bound(page->keys.begin(), page->keys.end(), key);
    const std::size_t index = static_cast<std::size_t>(pos - page->keys.begin());
    if (pos != page->keys.end() && *pos == key) {
        page->values[index] = value;
        return RCOK;
    }
    page->keys.insert(pos, key);
    page->values.insert(page->values.begin() + static_cast<std::ptrdiff_t>(index), value);
    size_.fetch_add(1, std::memory_order_relaxed);
    if (page->keys.size() < order_) return RCOK;

    idx_key_t separator = 0;
    std::unique_ptr<Page> right;
    split_leaf(*page, right, separator);
    // Everything above the lowest unsafe node is still latched, so the split
    // can be published without re-latching a parent that may have changed.
    for (std::size_t depth = path.size(); depth-- > 1;) {
        Page* parent = path[depth - 1];
        Page* child = path[depth];
        const std::size_t child_index = page_slot(*parent, child);
        if (child_index == parent->children.size()) return ERROR;
        parent->keys.insert(parent->keys.begin() + static_cast<std::ptrdiff_t>(child_index), separator);
        parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(child_index + 1),
                                std::move(right));
        parent->children[child_index + 1]->parent = parent;
        if (parent->keys.size() < order_) return RCOK;
        split_internal(*parent, right, separator);
    }
    // The root was checked to have room, so a root split here means the tree
    // changed underneath the shared guard; the caller retries.
    return WAIT;
}

RC BTreeLatch::insert_pessimistic(idx_key_t key, void* value) {
    // Root creation and root splits are the only structural steps that need the
    // tree-wide guard exclusively. Both are rare: the root gains one separator
    // per height increase.
    for (int attempt = 0; attempt < 64; ++attempt) {
        bool missing = false;
        bool full = false;
        {
            std::shared_lock<std::shared_mutex> probe(root_guard_);
            missing = root_ == nullptr;
            if (!missing) {
                // The size must be read under the page latch; the guard alone
                // only keeps the root pointer stable.
                std::shared_lock<std::shared_mutex> root_page(root_->latch);
                full = root_->keys.size() >= order_ - 1;
            }
        }
        if (missing) {
            std::unique_lock<std::shared_mutex> exclusive(root_guard_);
            if (root_ == nullptr) {
                auto page = make_page(true);
                page->keys.push_back(key);
                page->values.push_back(value);
                root_ = std::move(page);
                size_.store(1, std::memory_order_relaxed);
                return RCOK;
            }
            continue;
        }
        if (full) {
            std::unique_lock<std::shared_mutex> exclusive(root_guard_);
            return insert_forcing_root_split(key, value);
        }
        const RC rc = insert_descent(key, value);
        if (rc != WAIT) return rc;
    }
    return ERROR;
}

RC BTreeLatch::insert_forcing_root_split(idx_key_t key, void* value) {
    if (root_ == nullptr) {
        auto page = make_page(true);
        page->keys.push_back(key);
        page->values.push_back(value);
        root_ = std::move(page);
        size_.store(1, std::memory_order_relaxed);
        return RCOK;
    }
    idx_key_t separator = 0;
    auto right = insert_recursive(*root_, key, value, separator);
    if (right != nullptr) {
        auto new_root = make_page(false);
        new_root->keys.push_back(separator);
        new_root->children.push_back(std::move(root_));
        new_root->children.push_back(std::move(right));
        new_root->children[0]->parent = new_root.get();
        new_root->children[1]->parent = new_root.get();
        root_ = std::move(new_root);
    }
    return RCOK;
}

idx_key_t BTreeLatch::first_key(const Page& page) {
    const Page* current = &page;
    while (!current->is_leaf) current = current->children.front().get();
    return current->keys.empty() ? 0 : current->keys.front();
}

std::size_t BTreeLatch::page_slot(const Page& parent, const Page* child) {
    for (std::size_t i = 0; i < parent.children.size(); ++i)
        if (parent.children[i].get() == child) return i;
    return parent.children.size();
}

RC BTreeLatch::erase(idx_key_t key) {
    if (order_ == 0) return Abort;
    // Erase is a cold path: no workload in this repository generates deletes.
    // Taking the tree-wide guard exclusively keeps unlink and root collapse
    // obviously correct; a latched variant would need upgrade handling for the
    // parent rewrite. Deletes therefore must not be used from the hot loop.
    std::unique_lock<std::shared_mutex> exclusive(root_guard_);
    if (root_ == nullptr) return Abort;

    std::vector<Page*> path;
    std::vector<std::unique_lock<std::shared_mutex>> locks;
    path.reserve(8);
    locks.reserve(8);
    Page* page = root_.get();
    locks.emplace_back(page->latch);
    path.push_back(page);
    while (!page->is_leaf) {
        const auto pos = std::upper_bound(page->keys.begin(), page->keys.end(), key);
        page = page->children[static_cast<std::size_t>(pos - page->keys.begin())].get();
        locks.emplace_back(page->latch);
        path.push_back(page);
    }
    const auto pos = std::lower_bound(page->keys.begin(), page->keys.end(), key);
    if (pos == page->keys.end() || *pos != key) return Abort;
    const std::size_t index = static_cast<std::size_t>(pos - page->keys.begin());
    page->keys.erase(pos);
    page->values.erase(page->values.begin() + static_cast<std::ptrdiff_t>(index));
    size_.fetch_sub(1, std::memory_order_relaxed);

    // Pages removed from the tree must outlive their latch, so they are parked
    // here and destroyed after every lock has been released.
    std::vector<std::unique_ptr<Page>> retired;
    for (std::size_t depth = path.size(); depth-- > 1;) {
        Page* child = path[depth];
        Page* parent = path[depth - 1];
        const std::size_t child_index = page_slot(*parent, child);
        if (child_index == parent->children.size()) return ERROR;
        if (child->keys.empty() && (child->is_leaf || child->children.empty())) {
            // Unlinking an empty page keeps every reachable page non-empty,
            // which is what makes "separator == first key of the subtree" hold
            // without implementing a merge. Removing a leaf never changes any
            // node's depth, so the tree stays balanced.
            //
            // The same removal applies to a pass-through node whose only child
            // was itself an empty leaf: once the child goes, the node holds no
            // keys and no children, so it leaves the tree too. Walking upwards
            // lets that cascade one level at a time.
            if (child->is_leaf) {
                if (child->prev != nullptr) child->prev->next = child->next;
                if (child->next != nullptr) child->next->prev = child->prev;
            }
            std::unique_ptr<Page> removed = std::move(parent->children[child_index]);
            parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(child_index));
            if (child_index > 0)
                parent->keys.erase(parent->keys.begin() + static_cast<std::ptrdiff_t>(child_index - 1));
            else if (!parent->keys.empty())
                parent->keys.erase(parent->keys.begin());
            locks[depth].unlock();
            retired.push_back(std::move(removed));
            continue;
        }
        if (child->keys.empty() && child->children.size() != 1) return ERROR;
        // An internal node that lost every separator keeps its single child in
        // place. Splicing that child upwards would shorten this subtree by one
        // level while its siblings stay put, unbalancing the tree; the
        // pass-through node is left for a future merge pass instead. Its
        // separator must still follow the subtree's new minimum.
        if (child_index > 0)
            parent->keys[child_index - 1] = first_key(*child);
    }

    // Collapse a root that lost all of its separators: the tree height drops.
    // A root that also lost its last child leaves an empty tree behind.
    for (;;) {
        if (root_ == nullptr) break;
        if (!root_->is_leaf && root_->keys.empty() && root_->children.empty()) {
            std::unique_ptr<Page> old_root = std::move(root_);
            if (locks[0].owns_lock()) locks[0].unlock();
            retired.push_back(std::move(old_root));
            break;
        }
        if (!root_->is_leaf && root_->keys.empty() && root_->children.size() == 1) {
            std::unique_ptr<Page> old_root = std::move(root_);
            root_ = std::move(old_root->children.front());
            root_->parent = nullptr;
            if (locks[0].owns_lock()) locks[0].unlock();
            retired.push_back(std::move(old_root));
            continue;
        }
        break;
    }
    return RCOK;
}

bool BTreeLatch::lookup(idx_key_t key, void*& value) const {
    std::shared_lock<std::shared_mutex> root_lock(root_guard_);
    if (root_ == nullptr) return false;
    Page* page = root_.get();
    std::shared_lock<std::shared_mutex> page_lock(page->latch);
    while (!page->is_leaf) {
        const auto pos = std::upper_bound(page->keys.begin(), page->keys.end(), key);
        Page* child = page->children[static_cast<std::size_t>(pos - page->keys.begin())].get();
        std::shared_lock<std::shared_mutex> child_lock(child->latch);
        page_lock.unlock();
        page = child;
        page_lock = std::move(child_lock);
    }
    const auto pos = std::lower_bound(page->keys.begin(), page->keys.end(), key);
    if (pos == page->keys.end() || *pos != key) return false;
    value = page->values[static_cast<std::size_t>(pos - page->keys.begin())];
    return true;
}

bool BTreeLatch::range_ended(const KeyRangeEnt& range, idx_key_t key) {
    return range.has_end && (key > range.EndKey ||
           (key == range.EndKey && range.end_bound == Bound::Open));
}

std::vector<KeyValue> BTreeLatch::scan_range(const KeyRangeEnt& range) const {
    std::vector<KeyValue> result;
    if (range.empty() || !range.valid()) return result;
    std::shared_lock<std::shared_mutex> root_lock(root_guard_);
    if (root_ == nullptr) return result;
    Page* page = root_.get();
    std::shared_lock<std::shared_mutex> page_lock(page->latch);
    while (!page->is_leaf) {
        Page* child = range.has_start
            ? page->children[static_cast<std::size_t>(std::upper_bound(page->keys.begin(), page->keys.end(), range.StartKey) - page->keys.begin())].get()
            : page->children.front().get();
        std::shared_lock<std::shared_mutex> child_lock(child->latch);
        page_lock.unlock();
        page = child;
        page_lock = std::move(child_lock);
    }
    while (page != nullptr) {
        for (std::size_t i = 0; i < page->keys.size(); ++i) {
            if (range_ended(range, page->keys[i])) return result;
            if (range.contains(page->keys[i])) result.push_back({page->keys[i], page->values[i]});
        }
        Page* next = page->next;
        if (next == nullptr) break;
        std::shared_lock<std::shared_mutex> next_lock(next->latch);
        page_lock.unlock();
        page = next;
        page_lock = std::move(next_lock);
    }
    return result;
}

bool BTreeLatch::validate_page(const Page* page, const Page* expected_parent,
                               std::size_t depth, idx_key_t* min_key,
                               idx_key_t* max_key, std::size_t* leaf_count,
                               std::size_t* leaf_depth) const {
    if (page == nullptr) return false;
    std::shared_lock<std::shared_mutex> page_lock(page->latch);
    if (page->parent != expected_parent || page->keys.size() >= order_) return false;
    // Only the root may be empty; every other page must hold at least one key,
    // otherwise its separator would not identify it.
    if (expected_parent != nullptr && page->keys.empty() && page->is_leaf) return false;
    if (page->is_leaf) {
        if (page->keys.size() != page->values.size() || !std::is_sorted(page->keys.begin(), page->keys.end())) return false;
        if (page->next != nullptr && page->next->prev != page) return false;
        if (!page->keys.empty()) { *min_key = page->keys.front(); *max_key = page->keys.back(); }
        // Every leaf must sit at the same depth; catching this is what keeps a
        // delete from silently unbalancing one subtree.
        if (*leaf_count == 0) *leaf_depth = depth;
        else if (*leaf_depth != depth) return false;
        ++*leaf_count;
        return true;
    }
    if (page->children.size() != page->keys.size() + 1 || page->children.empty() || !std::is_sorted(page->keys.begin(), page->keys.end())) return false;
    // A separator-less internal node is only legal as a single-child pass-through.
    if (page->keys.empty() && page->children.size() != 1) return false;
    idx_key_t previous_max = 0;
    bool have_previous = false;
    for (std::size_t i = 0; i < page->children.size(); ++i) {
        idx_key_t child_min = 0, child_max = 0;
        if (!validate_page(page->children[i].get(), page, depth + 1, &child_min, &child_max,
                           leaf_count, leaf_depth)) return false;
        if (have_previous && previous_max >= child_min) return false;
        if (i < page->keys.size() && child_max >= page->keys[i]) return false;
        if (i > 0 && page->keys[i - 1] != child_min) return false;
        previous_max = child_max;
        have_previous = true;
        if (i == 0) *min_key = child_min;
        if (i + 1 == page->children.size()) *max_key = child_max;
    }
    return true;
}

bool BTreeLatch::validate() const {
    std::shared_lock<std::shared_mutex> root_lock(root_guard_);
    if (root_ == nullptr) return size_.load(std::memory_order_relaxed) == 0;
    idx_key_t min_key = 0, max_key = 0;
    std::size_t leaves = 0, leaf_depth = 0;
    if (!validate_page(root_.get(), nullptr, 0, &min_key, &max_key, &leaves, &leaf_depth))
        return false;
    const Page* leaf = root_.get();
    while (!leaf->is_leaf) leaf = leaf->children.front().get();
    const Page* previous = nullptr;
    std::size_t counted = 0;
    while (leaf != nullptr) {
        std::shared_lock<std::shared_mutex> leaf_lock(leaf->latch);
        if (leaf->prev != previous) return false;
        counted += leaf->keys.size();
        previous = leaf;
        leaf = leaf->next;
    }
    return counted == size_.load(std::memory_order_relaxed) && leaves > 0;
}

std::size_t BTreeLatch::size() const {
    return size_.load(std::memory_order_relaxed);
}

std::size_t BTreeLatch::height() const {
    std::shared_lock<std::shared_mutex> root_lock(root_guard_);
    std::size_t result = 0;
    for (const Page* page = root_.get(); page != nullptr; page = page->is_leaf ? nullptr : page->children.front().get()) ++result;
    return result;
}

bool BTreeLatch::leaf_cover_into(const KeyRangeEnt& range,std::vector<LeafSpan>& out) const {
    std::shared_lock<std::shared_mutex> guard(root_guard_);
    return detail::host_leaf_cover(root_.get(),range,out,true);
}

}  // namespace rrl
