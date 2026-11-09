#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "index/btree.h"

namespace rolex {
namespace port {

// Optional, algorithm-local counters.  They do not participate in the
// zongshu transaction/lock protocol.
struct RolexDiagnostics {
  std::uint64_t points{0}, ranges{0}, inserts{0}, updates{0}, erases{0};
  std::uint64_t model_predictions{0}, model_exact_pages{0},
      model_corrected_pages{0}, model_fallbacks{0};
  std::uint64_t leaf_splits{0}, leaf_removals{0}, leaf_allocations{0};
  std::uint64_t segment_lookups{0}, page_scans{0}, retrains{0};
  std::uint64_t hugepage_bytes{0}, arena_bytes{0}, live_keys{0};
  std::uint64_t seeds{0}, segments{0};
  // One-time PLR training + leaf materialisation.  Drivers that build lazily
  // include this inside their measured window; subtract it for steady state.
  std::uint64_t build_ns{0};
  bool huge_pages{false};
  bool huge_pages_available{false};
  bool huge_pages_requested{true};
  std::string mapping, atomic_backend, rdma_backend, fallback_reason;
};

// A ROLEX learned index adapted to rrl::BTreeIndex.
//
// Training follows FAST'23 section 3.2: OptimalPLR segments with an added
// prediction bias, fixed-size leaves, and data confined to a leaf (plus a
// split successor) so inserts never require retraining.  The physical leaf
// allocator and table-lock atomics of the RDMA path are lowered to the local
// primitives documented in hardware_compat.h.
class RolexIndex final : public rrl::BTreeIndex {
 public:
  explicit RolexIndex(std::size_t epsilon = 32, std::size_t leaf_slots = 64,
                      bool prefer_hugetlb = false, std::size_t max_segment_keys = 2048,
                      std::size_t arena_leaf_capacity = 0);
  ~RolexIndex() override;

  RolexIndex(const RolexIndex&) = delete;
  RolexIndex& operator=(const RolexIndex&) = delete;

  rrl::RC init(std::size_t order) override;
  rrl::RC insert(rrl::idx_key_t key, void* value) override;
  rrl::RC erase(rrl::idx_key_t key) override;
  bool lookup(rrl::idx_key_t key, void*& value) const override;
  std::vector<rrl::KeyValue> scan_range(
      const rrl::KeyRangeEnt& range) const override;
  void scan_range_into(const rrl::KeyRangeEnt& range,
                       std::vector<rrl::KeyValue>& out) const override;
  std::size_t size() const override;
  bool key_neighbors(rrl::idx_key_t, rrl::IndexKeyNeighbors&) const override;
  bool key_range_into(const rrl::KeyRangeEnt&, std::vector<rrl::idx_key_t>&) const override;
  rrl::RC apply_batch(const std::vector<rrl::IndexMutation>& changes) override;

  void prepare_thread(std::size_t worker,
                      std::size_t scan_capacity) override;
  bool healthy() const override;
  bool preloaded() const override { return false; }

  // Quiescent validation and diagnostics, analogous to BTreeGriffin.
  bool validate() const;
  void wait_ready() const;
  RolexDiagnostics diagnostics() const;
  void reset_diagnostics();

  // Diagnostic helper: every physical (segment, leaf) slot holding the key.
  // Used by the runner audit to explain a lookup miss; not on any hot path.
  std::vector<std::pair<std::size_t, std::size_t>> locate(
      rrl::idx_key_t key) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace port
}  // namespace rolex
