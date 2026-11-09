#include "index/rolex.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <thread>
#include <vector>

namespace {

using rrl::KeyRangeEnt;
using rolex::port::RolexIndex;

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

std::vector<rrl::KeyValue> expected(const std::map<std::uint64_t, std::uint64_t>& ref,
                                    const KeyRangeEnt& range) {
  std::vector<rrl::KeyValue> out;
  for (const auto& [key, value] : ref)
    if (range.contains(key)) out.push_back({key, const_cast<std::uint64_t*>(&value)});
  return out;
}

void check_scan(RolexIndex& index,
                const std::map<std::uint64_t, std::uint64_t>& ref,
                const KeyRangeEnt& range, const char* label) {
  const auto got = index.scan_range(range);
  const auto want = expected(ref, range);
  if (got.size() != want.size()) {
    std::cerr << "FAIL: " << label << " size got=" << got.size()
              << " want=" << want.size() << '\n';
    std::size_t i = 0;
    const std::size_t common = std::min(got.size(), want.size());
    while (i < common && got[i].key == want[i].key) ++i;
    std::cerr << "  first divergence at " << i;
    if (i < got.size()) std::cerr << " got=" << got[i].key;
    if (i < want.size()) std::cerr << " want=" << want[i].key;
    std::cerr << '\n';
    std::exit(1);
  }
  for (std::size_t i = 0; i < got.size(); ++i) {
    if (got[i].key != want[i].key) {
      std::cerr << "FAIL: " << label << " key[" << i << "] got=" << got[i].key
                << " want=" << want[i].key << '\n';
      std::exit(1);
    }
    const auto got_value = *static_cast<std::uint64_t*>(got[i].value);
    const auto want_value = *static_cast<std::uint64_t*>(want[i].value);
    if (got_value != want_value) {
      std::cerr << "FAIL: " << label << " value[" << i
                << "] key=" << got[i].key << " got=" << got_value
                << " want=" << want_value << '\n';
      std::exit(1);
    }
  }
}

}  // namespace

int main() {
  constexpr std::uint64_t kInitial = 50000;
  std::vector<std::uint64_t> values(200000);
  for (std::size_t i = 0; i < values.size(); ++i) values[i] = i ^ 0x9e3779b97f4a7c15ull;

  RolexIndex index(32, 64, true);
  require(index.init(128) == rrl::RCOK, "init");

  std::map<std::uint64_t, std::uint64_t> reference;
  for (std::uint64_t i = 0; i < kInitial; ++i) {
    reference.emplace(i, values[i]);
    require(index.insert(i, &values[i]) == rrl::RCOK, "initial insert");
  }
  require(index.size() == reference.size(), "initial size");
  index.prepare_thread(0, 32);
  require(index.validate(), "initial validate");

  const std::uint64_t probes[] = {0,        1,             63,   64,
                                  1023,     2047,          kInitial / 2,
                                  kInitial - 1};
  for (std::uint64_t key : probes) {
    void* value = nullptr;
    require(index.lookup(key, value), "lookup seeded key");
    require(value == &values[key], "lookup value");
  }

  check_scan(index, reference, KeyRangeEnt::half_open(0, 100), "scan [0,100)");
  check_scan(index, reference, KeyRangeEnt::closed(100, 200), "scan [100,200]");
  check_scan(index, reference, KeyRangeEnt::open(100, 200), "scan (100,200)");
  check_scan(index, reference, KeyRangeEnt::from_left(4090), "scan [4090,+inf)");
  check_scan(index, reference, KeyRangeEnt::to_right(100), "scan (-inf,100)");
  check_scan(index, reference, KeyRangeEnt::unbounded(), "scan all");

  // Cross a segment boundary and force fixed-leaf splits.
  for (std::uint64_t i = kInitial; i < 80000; ++i) {
    reference.emplace(i, values[i]);
    require(index.insert(i, &values[i]) == rrl::RCOK, "growth insert");
  }
  for (std::uint64_t i = 1; i < 20000; i += 2) {
    reference.erase(i);
    require(index.erase(i) == rrl::RCOK, "erase");
  }
  require(index.size() == reference.size(), "size after erase/insert");
  require(index.validate(), "validate after mutation");
  check_scan(index, reference, KeyRangeEnt::half_open(19000, 21000), "scan after mutation");

  // Batch is applied atomically from the index's point of view.
  const std::vector<rrl::IndexMutation> batch{
      {123456, &values[123456], false},
      {123457, &values[123457], false},
      {123456, &values[123458], false},
      {3, nullptr, true}};
  require(index.apply_batch(batch) == rrl::RCOK, "apply_batch");
  reference[123456] = values[123458];
  reference[123457] = values[123457];
  reference.erase(3);
  check_scan(index, reference, KeyRangeEnt::half_open(3, 123460), "scan after batch");

  // Concurrent readers and writers on disjoint key ranges.
  std::vector<std::thread> workers;
  for (std::size_t t = 0; t < 8; ++t) {
    workers.emplace_back([&, t] {
      std::mt19937_64 rng(t + 17);
      for (std::size_t i = 0; i < 3000; ++i) {
        const auto key = 100000 + t * 3000 + i;
        require(index.insert(key, &values[key % values.size()]) == rrl::RCOK,
                "concurrent insert");
        void* value = nullptr;
        require(index.lookup(key, value), "concurrent lookup");
        (void)index.scan_range(KeyRangeEnt::half_open(key, key + 16));
      }
    });
  }
  for (auto& worker : workers) worker.join();
  for (std::size_t t = 0; t < 8; ++t)
    for (std::size_t i = 0; i < 3000; ++i)
      reference.emplace(100000 + t * 3000 + i,
                        values[(100000 + t * 3000 + i) % values.size()]);
  require(index.size() == reference.size(), "concurrent size");
  require(index.validate(), "concurrent validate");

  const auto diagnostics = index.diagnostics();
  require(diagnostics.segments > 1, "PLR segments");
  require(diagnostics.leaf_allocations >= kInitial / 64, "leaf arena");
  require(diagnostics.model_predictions > 0, "model used");
  require(diagnostics.leaf_splits > 0, "leaf splitting exercised");
  std::cout << "rolex_index_test PASS segments=" << diagnostics.segments
            << " leaves=" << diagnostics.leaf_allocations
            << " live=" << diagnostics.live_keys
            << " splits=" << diagnostics.leaf_splits
            << " model_exact=" << diagnostics.model_exact_pages
            << " model_corrected=" << diagnostics.model_corrected_pages
            << " mapping=" << diagnostics.mapping << '\n';
  return 0;
}
