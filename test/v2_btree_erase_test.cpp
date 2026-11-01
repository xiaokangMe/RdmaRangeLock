#include "index/btree.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

using namespace rrl;

namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << "\n";
    return condition;
}

// Rebuilds the expected key set and compares it with a full scan.
bool scan_matches(BTreeLatch& tree, const std::vector<std::uint8_t>& present,
                  std::size_t key_count) {
    const auto rows = tree.scan_range(KeyRangeEnt::closed(0, key_count));
    std::size_t expected = 0;
    for (std::size_t key = 0; key < key_count; ++key) {
        if (!present[key]) continue;
        if (expected >= rows.size() || rows[expected].key != key) return false;
        ++expected;
    }
    return expected == rows.size();
}

}  // namespace

int main() {
    BTreeLatch tree;
    if (!check(tree.init(8) == RCOK, "init")) return 1;
    constexpr std::size_t kKeys = 4096;
    std::vector<std::uint64_t> values(kKeys);
    for (std::size_t key = 0; key < kKeys; ++key) values[key] = key + 7;
    std::vector<std::uint8_t> present(kKeys, 1);

    for (std::size_t key = 0; key < kKeys; ++key)
        if (!check(tree.insert(key, &values[key]) == RCOK, "seed insert")) return 1;
    if (!check(tree.size() == kKeys, "seed size")) return 1;
    if (!check(tree.validate(), "seed structure")) return 1;
    const std::size_t full_height = tree.height();
    if (!check(full_height >= 3, "seed height")) return 1;

    // Erasing a key that is not present must not change the tree.
    if (!check(tree.erase(kKeys + 100) == Abort, "missing erase rejects")) return 1;
    if (!check(tree.size() == kKeys, "missing erase size")) return 1;

    // Remove every other key: this forces empty leaves on the boundary between
    // retained ranges, which is what the unlink path has to clean up.
    for (std::size_t key = 1; key < kKeys; key += 2) {
        if (!check(tree.erase(key) == RCOK, "erase odd")) return 1;
        present[key] = 0;
    }
    if (!check(tree.size() == kKeys / 2, "odd erase size")) return 1;
    if (!check(tree.validate(), "odd erase structure")) return 1;
    if (!check(scan_matches(tree, present, kKeys), "odd erase scan")) return 1;

    // Drain the rest in random order, checking the invariants as the tree
    // shrinks. The height must never grow while only deletes happen.
    std::vector<std::size_t> order;
    order.reserve(kKeys);
    for (std::size_t key = 0; key < kKeys; ++key)
        if (present[key]) order.push_back(key);
    std::mt19937_64 random(20261101);
    std::shuffle(order.begin(), order.end(), random);
    std::size_t height = tree.height();
    std::size_t step = 0;
    for (const std::size_t key : order) {
        if (!check(tree.erase(key) == RCOK, "drain erase")) return 1;
        present[key] = 0;
        if ((++step % 64) != 0) continue;
        if (!check(tree.validate(), "drain structure")) return 1;
        const std::size_t current = tree.height();
        if (!check(current <= height, "height not growing under deletes")) return 1;
        height = current;
    }
    if (!check(tree.size() == 0, "drained size")) return 1;
    if (!check(tree.validate(), "drained structure")) return 1;

    // Refill after a full drain: the root must be usable again and rebuild the
    // height from scratch.
    for (std::size_t key = 0; key < kKeys; ++key)
        if (!check(tree.insert(key, &values[key]) == RCOK, "refill insert")) return 1;
    if (!check(tree.size() == kKeys, "refill size")) return 1;
    if (!check(tree.validate(), "refill structure")) return 1;
    if (!check(tree.height() == full_height, "refill height")) return 1;

    // Update path must not create duplicates after the structural changes.
    for (std::size_t key = 0; key < kKeys; ++key)
        if (!check(tree.insert(key, &values[key]) == RCOK, "update")) return 1;
    if (!check(tree.size() == kKeys, "update size")) return 1;

    std::cout << "v2 B+ tree erase/unlink: OK keys=" << kKeys
              << " height=" << full_height << "\n";
    return 0;
}
