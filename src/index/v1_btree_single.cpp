#include "index/btree.h"

#include <algorithm>

namespace rrl {

namespace { constexpr std::size_t kMinOrder = 3; }

RC BTreeSingle::init(std::size_t order) {
    if (order < kMinOrder) return ERROR;
    order_ = order;
    size_ = 0;
    root_.reset();
    return RCOK;
}

std::unique_ptr<BTreeSingle::Page> BTreeSingle::make_page(bool leaf) const {
    auto page = std::make_unique<Page>();
    page->is_leaf = leaf;
    page->keys.reserve(order_);
    if (leaf) page->values.reserve(order_);
    else page->children.reserve(order_ + 1);
    return page;
}

BTreeSingle::Page* BTreeSingle::find_leaf(idx_key_t key) const {
    Page* page = root_.get();
    while (page != nullptr && !page->is_leaf) {
        const auto pos = std::upper_bound(page->keys.begin(), page->keys.end(), key);
        page = page->children[static_cast<std::size_t>(pos - page->keys.begin())].get();
    }
    return page;
}

void BTreeSingle::split_leaf(Page& page, std::unique_ptr<Page>& right,
                             idx_key_t& separator) {
    right = make_page(true);
    right->parent = page.parent;
    const std::size_t split = page.keys.size() / 2;
    right->keys.assign(page.keys.begin() + static_cast<std::ptrdiff_t>(split), page.keys.end());
    right->values.assign(page.values.begin() + static_cast<std::ptrdiff_t>(split), page.values.end());
    page.keys.resize(split);
    page.values.resize(split);
    right->prev = &page;
    right->next = page.next;
    if (page.next != nullptr) page.next->prev = right.get();
    page.next = right.get();
    separator = right->keys.front();
}

void BTreeSingle::split_internal(Page& page, std::unique_ptr<Page>& right,
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

std::unique_ptr<BTreeSingle::Page> BTreeSingle::insert_recursive(
    Page& page, idx_key_t key, void* value, idx_key_t& separator) {
    if (page.is_leaf) {
        const auto pos = std::lower_bound(page.keys.begin(), page.keys.end(), key);
        const std::size_t index = static_cast<std::size_t>(pos - page.keys.begin());
        if (pos != page.keys.end() && *pos == key) {
            page.values[index] = value;
            return nullptr;
        }
        page.keys.insert(pos, key);
        page.values.insert(page.values.begin() + static_cast<std::ptrdiff_t>(index), value);
        ++size_;
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

RC BTreeSingle::insert(idx_key_t key, void* value) {
    if (order_ == 0) return ERROR;
    if (root_ == nullptr) {
        root_ = make_page(true);
        root_->keys.push_back(key);
        root_->values.push_back(value);
        size_ = 1;
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

bool BTreeSingle::refresh_separators(Page& page, idx_key_t& first_key) {
    if (page.is_leaf) {
        if (page.keys.empty()) return false;
        first_key = page.keys.front();
        return true;
    }
    bool have_first = false;
    idx_key_t child_first = 0;
    for (std::size_t i = 0; i < page.children.size(); ++i) {
        if (!refresh_separators(*page.children[i], child_first)) continue;
        if (!have_first) { first_key = child_first; have_first = true; }
        if (i > 0) page.keys[i - 1] = child_first;
    }
    return have_first;
}

RC BTreeSingle::erase(idx_key_t key) {
    Page* leaf = find_leaf(key);
    if (leaf == nullptr) return Abort;
    const auto pos = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
    if (pos == leaf->keys.end() || *pos != key) return Abort;
    const std::size_t index = static_cast<std::size_t>(pos - leaf->keys.begin());
    leaf->keys.erase(pos);
    leaf->values.erase(leaf->values.begin() + static_cast<std::ptrdiff_t>(index));
    --size_;
    idx_key_t first_key = 0;
    refresh_separators(*root_, first_key);
    return RCOK;
}

bool BTreeSingle::lookup(idx_key_t key, void*& value) const {
    Page* leaf = find_leaf(key);
    if (leaf == nullptr) return false;
    const auto pos = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
    if (pos == leaf->keys.end() || *pos != key) return false;
    value = leaf->values[static_cast<std::size_t>(pos - leaf->keys.begin())];
    return true;
}

bool BTreeSingle::range_ended(const KeyRangeEnt& range, idx_key_t key) {
    return range.has_end && (key > range.EndKey ||
           (key == range.EndKey && range.end_bound == Bound::Open));
}

std::vector<KeyValue> BTreeSingle::scan_range(const KeyRangeEnt& range) const {
    std::vector<KeyValue> result;
    if (root_ == nullptr || range.empty() || !range.valid()) return result;
    Page* leaf = range.has_start ? find_leaf(range.StartKey) : root_.get();
    if (leaf == nullptr) return result;
    while (!leaf->is_leaf) leaf = leaf->children.front().get();
    while (leaf != nullptr) {
        for (std::size_t i = 0; i < leaf->keys.size(); ++i) {
            if (range_ended(range, leaf->keys[i])) return result;
            if (range.contains(leaf->keys[i])) result.push_back({leaf->keys[i], leaf->values[i]});
        }
        leaf = leaf->next;
    }
    return result;
}

bool BTreeSingle::validate_page(const Page* page, const Page* expected_parent,
                                idx_key_t* min_key, idx_key_t* max_key,
                                std::size_t* leaf_count) const {
    if (page == nullptr || page->parent != expected_parent || page->keys.size() >= order_) return false;
    if (page->is_leaf) {
        if (page->keys.size() != page->values.size() || !std::is_sorted(page->keys.begin(), page->keys.end())) return false;
        if (page->next != nullptr && page->next->prev != page) return false;
        if (!page->keys.empty()) { *min_key = page->keys.front(); *max_key = page->keys.back(); }
        ++*leaf_count;
        return true;
    }
    if (page->children.size() != page->keys.size() + 1 || page->children.empty() || !std::is_sorted(page->keys.begin(), page->keys.end())) return false;
    idx_key_t previous_max = 0;
    bool have_previous = false;
    for (std::size_t i = 0; i < page->children.size(); ++i) {
        idx_key_t child_min = 0, child_max = 0;
        if (!validate_page(page->children[i].get(), page, &child_min, &child_max, leaf_count)) return false;
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

bool BTreeSingle::validate() const {
    if (root_ == nullptr) return size_ == 0;
    idx_key_t min_key = 0, max_key = 0;
    std::size_t leaves = 0;
    if (!validate_page(root_.get(), nullptr, &min_key, &max_key, &leaves)) return false;
    const Page* leaf = root_.get();
    while (!leaf->is_leaf) leaf = leaf->children.front().get();
    const Page* previous = nullptr;
    std::size_t counted = 0;
    while (leaf != nullptr) {
        if (leaf->prev != previous) return false;
        counted += leaf->keys.size();
        previous = leaf;
        leaf = leaf->next;
    }
    return counted == size_ && leaves > 0;
}

std::size_t BTreeSingle::height() const {
    std::size_t result = 0;
    for (const Page* page = root_.get(); page != nullptr; page = page->is_leaf ? nullptr : page->children.front().get()) ++result;
    return result;
}

}  // namespace rrl