// SPDX-License-Identifier: GPL-3.0-only
#include "index/rolex.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "adapter/hardware_compat.h"
#include "rolex/leaf.hpp"
#include "rolex/plr.hpp"

namespace rolex {
namespace port {
namespace {

using K = rrl::idx_key_t;
using V = std::uint64_t;
using LeafT = ::rolex::Leaf<64, K, V>;

constexpr std::size_t kLeafSlots = LeafT::max_slot();
static_assert(kLeafSlots == 64, "ROLEX port expects the official 64-slot leaf");
struct LeafArena {
  std::recursive_mutex allocation_mutex;
  MappedRegion region;
  LeafT* base{nullptr};
  std::uint64_t capacity{0};
  std::atomic<std::uint64_t> used{0};
  HardwareProfile profile;

  bool init(std::uint64_t initial_leaves, bool prefer_hugetlb, std::size_t requested) {
    capacity = requested ? requested : std::max<std::uint64_t>(1024, initial_leaves * 2 + 1024);
    if(capacity<initial_leaves || capacity>SIZE_MAX/sizeof(LeafT))return false;
    region = MappedRegion::create(capacity * sizeof(LeafT), prefer_hugetlb,
                                  &profile);
    if (!region.valid()) return false;
    base = static_cast<LeafT*>(region.data());
    profile.bytes = region.size();
    return true;
  }

  LeafT* allocate() {
    std::lock_guard<std::recursive_mutex> guard(allocation_mutex);
    const auto n = used.load(std::memory_order_relaxed);
    if (n >= capacity) return nullptr;
    used.store(n+1,std::memory_order_release);
    LeafT* leaf = base + n;
    new (leaf) LeafT();
    return leaf;
  }

  std::uint64_t remaining() const {
    const auto n = used.load(std::memory_order_acquire);
    return n >= capacity ? 0 : capacity - n;
  }
};

struct Segment {
  // f(x) = slope * x + intercept, trained on local positions [0, trained).
  double slope{0};
  double intercept{0};
  std::size_t trained{0};
  K max_key{0};
  mutable std::shared_mutex mutex;
  std::vector<LeafT*> pages;

  Segment(double s, double i, std::size_t n, K max)
      : slope(s), intercept(i), trained(n), max_key(max) {}

  std::size_t predict_page(K key) const {
    if (trained == 0 || pages.empty()) return 0;
    const long double raw =
        static_cast<long double>(slope) * static_cast<long double>(key) +
        static_cast<long double>(intercept);
    long double clamped = raw;
    if (!(clamped >= 0)) clamped = 0;
    if (clamped > static_cast<long double>(trained - 1))
      clamped = static_cast<long double>(trained - 1);
    const auto pos = static_cast<std::size_t>(clamped);
    const auto page = pos / kLeafSlots;
    return std::min(page, pages.size() - 1);
  }

  // The PLR prediction is a hint after splits.  A short walk repairs the
  // common case; a binary search over leaf fences is the guaranteed fallback.
  std::size_t page_for_key(K key, bool* model_exact, bool* fallback) const {
    if (pages.empty()) return 0;
    std::size_t idx = predict_page(key);
    const auto start = idx;
    std::size_t steps = 0;
    while (idx + 1 < pages.size() && key > pages[idx]->last_valid_key() &&
           steps++ < 16) {
      ++idx;
    }
    while (idx > 0 && key < pages[idx]->first_key() && steps++ < 16) {
      --idx;
    }
    const bool within =
        !pages[idx]->isEmpty() && key >= pages[idx]->first_key() &&
        key <= pages[idx]->last_valid_key();
    if (!within || steps > 16) {
      if (fallback) *fallback = true;
      // First leaf whose last key is >= key.  (upper_bound here would land on
      // the previous leaf and silently miss the key.)
      const auto it = std::lower_bound(
          pages.begin(), pages.end(), key,
          [](const LeafT* leaf, K wanted) {
            return leaf->last_valid_key() < wanted;
          });
      idx = it == pages.end() ? pages.size() - 1
                              : static_cast<std::size_t>(it - pages.begin());
      if (model_exact) *model_exact = false;
    } else if (model_exact) {
      *model_exact = steps == 0 && idx == start;
    }
    return idx;
  }
};

struct Counters {
  std::atomic<std::uint64_t> points{0}, ranges{0}, inserts{0}, updates{0},
      erases{0}, model_predictions{0}, model_exact_pages{0},
      model_corrected_pages{0}, model_fallbacks{0}, leaf_splits{0},
      leaf_removals{0}, segment_lookups{0}, page_scans{0}, retrains{0},
      seeds{0};
};

}  // namespace

struct RolexIndex::Impl {
  explicit Impl(std::size_t eps, bool huge,std::size_t segment_keys,std::size_t arena_leaves)
      : epsilon(eps), prefer_hugetlb(huge),max_segment_keys(segment_keys),arena_leaf_capacity(arena_leaves) {}

  const std::size_t epsilon;
  const bool prefer_hugetlb;
  const std::size_t max_segment_keys,arena_leaf_capacity;
  bool initialized{false};
  std::atomic<bool> ready{false};
  std::atomic<bool> failed{false};
  mutable std::once_flag build_once;
  mutable std::mutex build_mutex;
  std::map<K, void*> staged;
  std::atomic<std::size_t> staged_size{0};
  std::atomic<std::size_t> live_keys{0};
  std::atomic<std::uint64_t> build_ns{0};

  LeafArena arena;
  std::vector<std::unique_ptr<Segment>> segments;
  std::vector<K> model_keys;
  mutable Counters counters;
  std::size_t order_hint{0};

  // ------------------------------------------------------------------
  // Training
  // ------------------------------------------------------------------
  void build() const {
    std::lock_guard<std::mutex> build_guard(build_mutex);
    if (ready.load(std::memory_order_acquire)) return;
    auto* self = const_cast<Impl*>(this);
    const auto build_start = std::chrono::steady_clock::now();
    const auto finish = [&] {
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - build_start)
                               .count();
      self->build_ns.store(static_cast<std::uint64_t>(elapsed));
      self->ready.store(true, std::memory_order_release);
    };

    std::vector<std::pair<K, void*>> sorted;
    sorted.reserve(staged.size());
    for (const auto& entry : staged) sorted.emplace_back(entry.first, entry.second);
    self->staged.clear();

    const std::uint64_t initial_leaves =
        (sorted.size() + kLeafSlots - 1) / kLeafSlots;
    if (!self->arena.init(initial_leaves, prefer_hugetlb,arena_leaf_capacity)) {
      self->failed.store(true, std::memory_order_release);
      throw std::runtime_error("ROLEX leaf arena allocation failed");
    }

    if(sorted.empty()) {
      self->segments.push_back(std::make_unique<Segment>(0,0,0,UINT64_MAX));
      self->model_keys.push_back(UINT64_MAX);
      finish();return;
    }
    std::size_t start = 0;
    while (start < sorted.size()) {
      const std::size_t hard_end =
          std::min(sorted.size(), start + max_segment_keys);
      auto plr = std::make_unique<::rolex::PLR<K, std::size_t>>(
          epsilon > 0 ? epsilon - 1 : 0);
      plr->add_point(sorted[start].first, 0);

      std::size_t segment_start = start;
      std::size_t i = start + 1;
      for (; i < hard_end; ++i) {
        if (!plr->add_point(sorted[i].first, i - segment_start)) {
          // Finalize [segment_start, i) and start a new model at i.
          self->append_segment(sorted, segment_start, i, *plr);
          segment_start = i;
          plr = std::make_unique<::rolex::PLR<K, std::size_t>>(
              epsilon > 0 ? epsilon - 1 : 0);
          plr->add_point(sorted[i].first, 0);
        }
      }
      self->append_segment(sorted, segment_start, hard_end, *plr);
      start = hard_end;
    }

    self->live_keys.store(sorted.size(), std::memory_order_release);
    self->counters.seeds.store(sorted.size(), std::memory_order_release);
    finish();
  }

  void append_segment(const std::vector<std::pair<K, void*>>& sorted,
                      std::size_t begin, std::size_t end,
                      ::rolex::PLR<K, std::size_t>& plr) {
    if (begin >= end) return;
    const auto [slope_ld, intercept_ld] = plr.get_segment().get_slope_intercept();
    auto segment = std::make_unique<Segment>(
        static_cast<double>(slope_ld), static_cast<double>(intercept_ld),
        end - begin, sorted[end - 1].first);

    for (std::size_t p = begin; p < end; p += kLeafSlots) {
      LeafT* leaf = arena.allocate();
      if (!leaf) {
        failed.store(true, std::memory_order_release);
        throw std::runtime_error("ROLEX leaf arena exhausted during training");
      }
      const std::size_t stop = std::min(end, p + kLeafSlots);
      for (std::size_t j = p; j < stop; ++j) {
        leaf->keys[j - p] = sorted[j].first;
        leaf->vals[j - p] = reinterpret_cast<V>(sorted[j].second);
        ++leaf->used_slots;
      }
      segment->pages.push_back(leaf);
    }
    model_keys.push_back(segment->max_key);
    segments.push_back(std::move(segment));
  }

  void ensure_ready() const {
    if (ready.load(std::memory_order_acquire)) return;
    std::call_once(build_once, [this] { build(); });
  }

  bool healthy() const {
    return initialized && !failed.load(std::memory_order_acquire);
  }

  std::size_t route_segment(K key) const {
    if (segments.empty()) return 0;
    // A segment owns every key up to and including its trained max_key, so the
    // first segment with max_key >= key is the owner.  upper_bound would send
    // key == max_key to the next segment and lose the boundary key.
    const auto it =
        std::lower_bound(model_keys.begin(), model_keys.end(), key);
    if (it == model_keys.end()) return segments.size() - 1;
    return static_cast<std::size_t>(it - model_keys.begin());
  }

  Segment* segment_for(K key, std::size_t* index = nullptr) {
    const auto idx = route_segment(key);
    if (index) *index = idx;
    return segments[idx].get();
  }

  const Segment* segment_for(K key, std::size_t* index = nullptr) const {
    const auto idx = route_segment(key);
    if (index) *index = idx;
    return segments[idx].get();
  }

  bool lookup_locked(Segment& segment, K key, void*& value,
                     bool* exact = nullptr, bool* fallback = nullptr) noexcept {
    if (segment.pages.empty()) return false;
    const auto page_index =
        segment.page_for_key(key, exact, fallback);
    ++counters.page_scans;
    V result = 0;
    if (!segment.pages[page_index]->search(key, result)) return false;
    value = reinterpret_cast<void*>(result);
    return true;
  }

  // Caller holds segment.mutex in shared mode.  Only a value slot changes, so
  // no key ever moves and readers are never blocked behind an update stampede.
  bool update_existing(Segment& segment, K key, void* value) {
    if (segment.pages.empty()) return false;
    bool exact = false, fallback = false;
    const auto page_index = segment.page_for_key(key, &exact, &fallback);
    auto* page = segment.pages[page_index];
    const auto slots = page->count();
    const V incoming = reinterpret_cast<V>(value);
    for (std::size_t i = 0; i < slots; ++i) {
      if (page->keys[i] == key) {
        page->store_value(i, incoming);
        ++counters.updates;
        return true;
      }
    }
    return false;
  }

  // Caller holds segment.mutex exclusively.
  bool insert_locked(Segment& segment, K key, void* value) {
    // Reserve before changing leaf keys: failed allocation cannot publish
    // half of a split. Batch callers reserve the upper bound in advance.
    if(segment.pages.size()==segment.pages.capacity())segment.pages.reserve(segment.pages.size()+1);
    if (segment.pages.empty()) {
      LeafT* leaf = arena.allocate();
      if (!leaf) return false;
      segment.pages.push_back(leaf);
    }
    bool exact = false, fallback = false;
    const auto page_index = segment.page_for_key(key, &exact, &fallback);
    auto* page = segment.pages[page_index];
    V found = 0;
    if (page->search(key, found)) {
      // Leaf is __attribute__((packed)), so walk the slots by index instead of
      // forming a reference/pointer into the packed key array.
      const auto slots = page->count();
      for (std::size_t i = 0; i < slots; ++i) {
        if (page->keys[i] == key) {
          page->store_value(i, reinterpret_cast<V>(value));
          ++counters.updates;
          return true;
        }
      }
      return false;
    }
    if (!page->isfull()) {
      page->insert_not_full(key, reinterpret_cast<V>(value));
      if (key > segment.max_key) segment.max_key = key;
      ++counters.inserts;
      ++live_keys;
      return true;
    }

    // Full fixed-size leaf: preserve ROLEX's no-retraining rule by allocating
    // a split successor (the local analogue of a synonym leaf) and keeping
    // every key inside this model's predicted leaf range.
    std::array<std::pair<K, V>, kLeafSlots + 1> all;
    const auto n = page->count();
    for (std::size_t i = 0; i < n; ++i) {
      // Copy out of the packed slot before building the pair: binding a
      // reference to a packed member is ill-formed.
      const K slot_key = page->keys[i];
      const V slot_val = page->vals[i];
      all[i] = std::make_pair(slot_key, slot_val);
    }
    const V incoming = reinterpret_cast<V>(value);
    all[n] = std::make_pair(key, incoming);
    std::sort(all.begin(), all.begin() + n + 1,
              [](const auto& a, const auto& b) { return a.first < b.first; });

    LeafT* successor = arena.allocate();
    if (!successor) return false;

    *page = LeafT();
    const std::size_t left = (n + 1) / 2;
    for (std::size_t i = 0; i < left; ++i) {
      page->keys[i] = all[i].first;
      page->vals[i] = all[i].second;
      ++page->used_slots;
    }
    for (std::size_t i = left; i < n + 1; ++i) {
      successor->keys[i - left] = all[i].first;
      successor->vals[i - left] = all[i].second;
      ++successor->used_slots;
    }
    segment.pages.insert(segment.pages.begin() + page_index + 1, successor);
    if (key > segment.max_key) segment.max_key = key;
    ++counters.leaf_splits;
    ++counters.inserts;
    ++live_keys;
    return true;
  }

  // Caller holds segment.mutex exclusively.
  bool erase_locked(Segment& segment, K key) {
    if (segment.pages.empty()) return false;
    bool exact = false, fallback = false;
    const auto page_index = segment.page_for_key(key, &exact, &fallback);
    auto* page = segment.pages[page_index];
    if (!page->remove(key)) return false;
    if (page->isEmpty()) {
      segment.pages.erase(segment.pages.begin() + page_index);
      ++counters.leaf_removals;
      // The fixed leaf is intentionally not reused: this mirrors ROLEX's
      // alloc_num-only leaf region and avoids ABA on stale references.
    }
    --live_keys;
    ++counters.erases;
    return true;
  }

  // Returns true when the segment already covers the range end, i.e. every key
  // in the following segments is past the range and the caller can stop.
  // Segment::max_key is frozen at training time and is therefore useless here
  // once inserts have pushed past it.
  bool scan_segment(const Segment& segment, const rrl::KeyRangeEnt& range,
                    std::vector<rrl::KeyValue>& out) const {
    if (segment.pages.empty()) return false;
    const bool has_start = range.has_start;
    const bool has_end = range.has_end;
    const auto start = has_start ? range.StartKey : std::numeric_limits<K>::min();
    const auto end = has_end ? range.EndKey : std::numeric_limits<K>::max();
    const bool end_inclusive = range.end_bound == rrl::Bound::Closed;

    std::size_t page_index = 0;
    if (has_start) {
      bool exact = false, fallback = false;
      page_index = segment.page_for_key(start, &exact, &fallback);
    }
    for (std::size_t p = page_index; p < segment.pages.size(); ++p) {
      auto* leaf = segment.pages[p];
      if (has_end && !leaf->isEmpty() &&
          (leaf->first_key() > end ||
           (!end_inclusive && leaf->first_key() == end)))
        return true;
      std::vector<std::pair<K, V>> rows;
      leaf->range_kv(start, has_end, end, end_inclusive, rows);
      for (const auto& row : rows) {
        if (range.contains(row.first)) {
          out.push_back({row.first, reinterpret_cast<void*>(row.second)});
        }
      }
      ++counters.page_scans;
      if (has_end) {
        const K last = leaf->last_valid_key();
        if (leaf->isEmpty() || last >= end) return true;
      }
    }
    return false;
  }

  RolexDiagnostics snapshot() const {
    RolexDiagnostics d;
    d.points = counters.points.load(std::memory_order_relaxed);
    d.ranges = counters.ranges.load(std::memory_order_relaxed);
    d.inserts = counters.inserts.load(std::memory_order_relaxed);
    d.updates = counters.updates.load(std::memory_order_relaxed);
    d.erases = counters.erases.load(std::memory_order_relaxed);
    d.model_predictions =
        counters.model_predictions.load(std::memory_order_relaxed);
    d.model_exact_pages =
        counters.model_exact_pages.load(std::memory_order_relaxed);
    d.model_corrected_pages =
        counters.model_corrected_pages.load(std::memory_order_relaxed);
    d.model_fallbacks =
        counters.model_fallbacks.load(std::memory_order_relaxed);
    d.leaf_splits = counters.leaf_splits.load(std::memory_order_relaxed);
    d.leaf_removals = counters.leaf_removals.load(std::memory_order_relaxed);
    d.leaf_allocations = arena.used.load(std::memory_order_relaxed);
    d.segment_lookups =
        counters.segment_lookups.load(std::memory_order_relaxed);
    d.page_scans = counters.page_scans.load(std::memory_order_relaxed);
    d.retrains = counters.retrains.load(std::memory_order_relaxed);
    d.seeds = counters.seeds.load(std::memory_order_relaxed);
    d.segments = segments.size();
    d.live_keys = live_keys.load(std::memory_order_relaxed);
    d.build_ns = build_ns.load(std::memory_order_relaxed);
    d.hugepage_bytes = arena.region.huge() ? arena.region.size() : 0;
    d.arena_bytes = arena.region.size();
    d.huge_pages = arena.profile.hugetlb_used;
    d.huge_pages_available = arena.profile.hugetlb_available;
    d.huge_pages_requested = prefer_hugetlb;
    d.mapping = arena.profile.mapping;
    d.atomic_backend = arena.profile.atomic_backend;
    d.rdma_backend = arena.profile.rdma_backend;
    d.fallback_reason = arena.profile.fallback_reason;
    return d;
  }
};

RolexIndex::RolexIndex(std::size_t epsilon, std::size_t leaf_slots,
                       bool prefer_hugetlb, std::size_t max_segment_keys,
                       std::size_t arena_leaf_capacity)
    : impl_(new Impl(epsilon, prefer_hugetlb,max_segment_keys,arena_leaf_capacity)) {
  if(!max_segment_keys || max_segment_keys>16777216 || epsilon>16777216)
    throw std::invalid_argument("ROLEX geometry bounds");
  if (leaf_slots != kLeafSlots)
    throw std::invalid_argument(
        "zongshu ROLEX port uses the official 64-slot Leaf layout");
}

RolexIndex::~RolexIndex() = default;

rrl::RC RolexIndex::init(std::size_t order) {
  if (impl_->initialized) return rrl::ERROR;
  if (order < 3) return rrl::ERROR;
  impl_->order_hint = order;
  impl_->initialized = true;
  return rrl::RCOK;
}

rrl::RC RolexIndex::insert(K key, void* value) {
  if (!impl_->initialized || !value) return rrl::ERROR;
  if (!impl_->ready.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> guard(impl_->build_mutex);
    if (!impl_->ready.load(std::memory_order_acquire)) {
      const auto [it, inserted] = impl_->staged.emplace(key, value);
      if (inserted) {
        impl_->staged_size.fetch_add(1, std::memory_order_relaxed);
        return rrl::RCOK;
      }
      // A repeated key means the bulk seed is over and the workload started
      // (seeding only ever inserts fresh keys).  Materialise the model now so
      // write-only workloads do not stay stuck on the staging map, which is
      // serialised by build_mutex.
      it->second = value;
    }
  }
  impl_->ensure_ready();
  std::size_t index = 0;
  auto* segment = impl_->segment_for(key, &index);
  ++impl_->counters.segment_lookups;
  {
    // YCSB-style updates (key already present) only rewrite a value slot, so a
    // shared lock suffices and mixes with readers instead of serialising them.
    std::shared_lock<std::shared_mutex> shared(segment->mutex);
    if (impl_->update_existing(*segment, key, value)) return rrl::RCOK;
  }
  std::unique_lock<std::shared_mutex> lock(segment->mutex);
  return impl_->insert_locked(*segment, key, value) ? rrl::RCOK : rrl::ERROR;
}

rrl::RC RolexIndex::erase(K key) {
  if (!impl_->initialized) return rrl::ERROR;
  if (!impl_->ready.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> guard(impl_->build_mutex);
    if (!impl_->ready.load(std::memory_order_acquire)) {
      const auto erased = impl_->staged.erase(key);
      if (erased) impl_->staged_size.fetch_sub(1, std::memory_order_relaxed);
      return erased ? rrl::RCOK : rrl::Abort;
    }
  }
  impl_->ensure_ready();
  std::size_t index = 0;
  auto* segment = impl_->segment_for(key, &index);
  ++impl_->counters.segment_lookups;
  std::unique_lock<std::shared_mutex> lock(segment->mutex);
  return impl_->erase_locked(*segment, key) ? rrl::RCOK : rrl::Abort;
}

bool RolexIndex::lookup(K key, void*& value) const {
  if (!impl_->initialized || !impl_->healthy()) return false;
  impl_->ensure_ready();
  if (impl_->segments.empty()) return false;
  std::size_t index = 0;
  const auto* segment = impl_->segment_for(key, &index);
  ++impl_->counters.segment_lookups;
  ++impl_->counters.points;
  std::shared_lock<std::shared_mutex> lock(segment->mutex);
  bool exact = false, fallback = false;
  ++impl_->counters.model_predictions;
  const bool found = impl_->lookup_locked(*const_cast<Segment*>(segment), key,
                                         value, &exact, &fallback);
  if (exact) ++impl_->counters.model_exact_pages;
  else if (fallback) ++impl_->counters.model_fallbacks;
  else ++impl_->counters.model_corrected_pages;
  return found;
}

std::vector<rrl::KeyValue> RolexIndex::scan_range(
    const rrl::KeyRangeEnt& range) const {
  std::vector<rrl::KeyValue> out;
  scan_range_into(range, out);
  return out;
}

void RolexIndex::scan_range_into(const rrl::KeyRangeEnt& range,
                                 std::vector<rrl::KeyValue>& out) const {
  out.clear();
  if (!impl_->initialized || !impl_->healthy() || range.empty()) return;
  // Build before testing segments: a scan-only workload never calls lookup, so
  // this is the first access that must materialise the trained index.
  impl_->ensure_ready();
  if (impl_->segments.empty()) return;
  ++impl_->counters.ranges;
  std::size_t start_index = 0;
  if (range.has_start) start_index = impl_->route_segment(range.StartKey);
  for (std::size_t i = start_index; i < impl_->segments.size(); ++i) {
    const auto& segment = *impl_->segments[i];
    bool done = false;
    {
      std::shared_lock<std::shared_mutex> lock(segment.mutex);
      done = impl_->scan_segment(segment, range, out);
    }
    if (done) break;
  }
}

std::size_t RolexIndex::size() const {
  if (!impl_->ready.load(std::memory_order_acquire))
    return impl_->staged_size.load(std::memory_order_relaxed);
  return impl_->live_keys.load(std::memory_order_relaxed);
}

rrl::RC RolexIndex::apply_batch(
    const std::vector<rrl::IndexMutation>& changes) {
  if (!impl_->initialized) return rrl::ERROR;
  if (changes.empty()) return rrl::RCOK;
  for(const auto& change:changes)if(!change.remove && !change.value)return rrl::ERROR;
  impl_->ensure_ready();
  if (impl_->segments.empty()) return rrl::ERROR;

  std::vector<std::size_t> indices;
  indices.reserve(changes.size());
  for (const auto& change : changes)
    indices.push_back(impl_->route_segment(change.key));
  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

  std::vector<std::unique_lock<std::shared_mutex>> guards;
  guards.reserve(indices.size());
  for (const auto index : indices)
    guards.emplace_back(impl_->segments[index]->mutex);

  std::size_t possible_new=0;
  for(const auto& change:changes)if(!change.remove) {
    void* old=nullptr;auto& segment=*impl_->segments[impl_->route_segment(change.key)];
    bool deleted=false;
    for(const auto& prior:changes){if(&prior==&change)break;if(prior.key==change.key && prior.remove)deleted=true;}
    if(deleted || !impl_->lookup_locked(segment,change.key,old))++possible_new;
  }
  std::unique_lock<std::recursive_mutex> allocation_guard(impl_->arena.allocation_mutex,std::defer_lock);
  if(possible_new) {
    allocation_guard.lock();
    if(impl_->arena.remaining()<possible_new)return rrl::ERROR;
    for(const auto index:indices) {
      auto& pages=impl_->segments[index]->pages;
      pages.reserve(pages.size()+possible_new);
    }
  }

  for (const auto& change : changes) {
    auto* segment = impl_->segments[impl_->route_segment(change.key)].get();
    if (change.remove) {
      impl_->erase_locked(*segment, change.key);
    } else {
      if (!change.value) return rrl::ERROR;
      if (!impl_->insert_locked(*segment, change.key,
                                const_cast<std::uint64_t*>(change.value)))
        return rrl::ERROR;
    }
  }
  return rrl::RCOK;
}

bool RolexIndex::key_range_into(const rrl::KeyRangeEnt& range,std::vector<K>& out) const {
  std::vector<rrl::KeyValue> rows;scan_range_into(range,rows);out.clear();out.reserve(rows.size());
  for(const auto& row:rows)out.push_back(row.key);return healthy();
}
bool RolexIndex::key_neighbors(K key,rrl::IndexKeyNeighbors& out) const {
  out={};if(!healthy())return false;impl_->ensure_ready();
  const auto home=impl_->route_segment(key);
  auto visit=[&](std::size_t i) {
    const auto& segment=*impl_->segments[i];std::shared_lock<std::shared_mutex> guard(segment.mutex);
    // Structural metadata only: the driver locks and revalidates these keys.
    for(const auto* leaf:segment.pages) {
      const auto n=leaf->count();
      const auto* it=std::lower_bound(leaf->keys,leaf->keys+n,key);
      const auto at=static_cast<std::size_t>(it-leaf->keys);
      if(at<n && leaf->keys[at]==key)out.present=true;
      if(at>0) {
        const auto candidate=leaf->keys[at-1];
        if(!out.has_predecessor || candidate>out.predecessor){out.predecessor=candidate;out.has_predecessor=true;}
      }
      auto after=at;if(after<n && leaf->keys[after]==key)++after;
      if(after<n) {
        const auto candidate=leaf->keys[after];
        if(!out.has_successor || candidate<out.successor){out.successor=candidate;out.has_successor=true;}
      }
    }
  };
  visit(home);
  for(auto i=home;i && !out.has_predecessor;)visit(--i);
  for(auto i=home+1;i<impl_->segments.size() && !out.has_successor;++i)visit(i);
  return healthy();
}

void RolexIndex::prepare_thread(std::size_t, std::size_t) {
  if (!impl_->initialized) return;
  impl_->ensure_ready();
}

bool RolexIndex::healthy() const {
  return impl_->initialized && !impl_->failed.load(std::memory_order_acquire);
}

bool RolexIndex::validate() const {
  if (!impl_->initialized || !impl_->healthy()) return false;
  if (!impl_->ready.load(std::memory_order_acquire)) return true;

  std::size_t live = 0;
  K previous_max = 0;
  bool have_previous = false;
  for (std::size_t s = 0; s < impl_->segments.size(); ++s) {
    const auto& segment = *impl_->segments[s];
    std::shared_lock<std::shared_mutex> lock(segment.mutex);
    if (segment.pages.empty()) continue;
    K previous_leaf_last = 0;
    bool have_leaf = false;
    for (const auto* leaf : segment.pages) {
      if (leaf->isEmpty()) return false;
      const auto n = leaf->count();
      if (n == 0 || n > kLeafSlots) return false;
      for (std::size_t i = 0; i < n; ++i) {
        if (i && leaf->keys[i] <= leaf->keys[i - 1]) return false;
      }
      if (have_leaf && leaf->first_key() <= previous_leaf_last) return false;
      if (have_previous && leaf->first_key() <= previous_max && s > 0)
        return false;
      previous_leaf_last = leaf->last_valid_key();
      have_leaf = true;
      live += n;
    }
    if (have_previous && segment.max_key <= previous_max) return false;
    previous_max = segment.max_key;
    have_previous = true;
  }
  return live == impl_->live_keys.load(std::memory_order_acquire);
}

void RolexIndex::wait_ready() const { impl_->ensure_ready(); }

std::vector<std::pair<std::size_t, std::size_t>> RolexIndex::locate(
    rrl::idx_key_t key) const {
  std::vector<std::pair<std::size_t, std::size_t>> hits;
  if (!impl_->initialized) return hits;
  impl_->ensure_ready();
  for (std::size_t s = 0; s < impl_->segments.size(); ++s) {
    const auto& segment = *impl_->segments[s];
    std::shared_lock<std::shared_mutex> lock(segment.mutex);
    for (std::size_t p = 0; p < segment.pages.size(); ++p) {
      auto* leaf = segment.pages[p];
      if (leaf->contain(key)) hits.emplace_back(s, p);
    }
  }
  return hits;
}

RolexDiagnostics RolexIndex::diagnostics() const { return impl_->snapshot(); }

void RolexIndex::reset_diagnostics() {
  impl_->counters.points.store(0);
  impl_->counters.ranges.store(0);
  impl_->counters.inserts.store(0);
  impl_->counters.updates.store(0);
  impl_->counters.erases.store(0);
  impl_->counters.model_predictions.store(0);
  impl_->counters.model_exact_pages.store(0);
  impl_->counters.model_corrected_pages.store(0);
  impl_->counters.model_fallbacks.store(0);
  impl_->counters.leaf_splits.store(0);
  impl_->counters.leaf_removals.store(0);
  impl_->counters.segment_lookups.store(0);
  impl_->counters.page_scans.store(0);
  impl_->counters.retrains.store(0);
}

}  // namespace port
}  // namespace rolex
