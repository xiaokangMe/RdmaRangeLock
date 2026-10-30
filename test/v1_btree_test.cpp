#include "index/btree.h"

#include <cstdint>
#include <iostream>
#include <vector>

using namespace rrl;

int main() {
    auto require = [](bool condition, const char* message) {
        if (!condition) { std::cerr << "FAIL: " << message << "\n"; return false; }
        return true;
    };

    BTreeSingle tree;
    if (!require(tree.init(4) == RCOK, "init order 4") ||
        !require(tree.init(2) == ERROR, "reject order 2") ||
        !require(tree.init(4) == RCOK, "reinit")) return 1;

    std::vector<std::uint64_t> values(128);
    for (std::uint64_t key = 0; key < values.size(); ++key) {
        values[key] = key * 10;
        if (!require(tree.insert(key, &values[key]) == RCOK, "insert")) return 1;
    }
    if (!require(tree.size() == values.size(), "size after insert") ||
        !require(tree.height() >= 3, "height after splits") ||
        !require(tree.validate(), "validate after insert")) return 1;

    for (std::uint64_t key : {0ULL, 1ULL, 63ULL, 127ULL}) {
        void* value = nullptr;
        if (!require(tree.lookup(key, value), "lookup existing") ||
            !require(value != nullptr && *static_cast<std::uint64_t*>(value) == key * 10,
                     "lookup value")) return 1;
    }
    void* absent = nullptr;
    if (!require(!tree.lookup(128, absent), "lookup absent")) return 1;

    const auto result = tree.scan_range(KeyRangeEnt::closed(10, 19));
    if (!require(result.size() == 10, "closed range count")) return 1;
    for (std::size_t i = 0; i < result.size(); ++i)
        if (!require(result[i].key == i + 10, "closed range order")) return 1;
    if (!require(tree.scan_range(KeyRangeEnt::half_open(10, 10)).empty(), "empty range") ||
        !require(tree.scan_range(KeyRangeEnt::from_left(120)).size() == 8, "left unbounded") ||
        !require(tree.scan_range(KeyRangeEnt::to_right(7, Bound::Closed)).size() == 8, "right bounded")) return 1;

    values[42] = 999;
    if (!require(tree.insert(42, &values[42]) == RCOK, "replace duplicate") ||
        !require(tree.size() == values.size(), "duplicate size")) return 1;
    void* updated = nullptr;
    if (!require(tree.lookup(42, updated) && updated != nullptr &&
                 *static_cast<std::uint64_t*>(updated) == 999, "updated value")) return 1;

    for (std::uint64_t key = 0; key < values.size(); key += 3) {
        if (!require(tree.erase(key) == RCOK, "erase") ||
            !require(tree.validate(), "validate after erase")) return 1;
    }
    if (!require(tree.size() == 85, "size after erase") ||
        !require(tree.erase(0) == Abort, "erase absent") ||
        !require(tree.validate(), "final validate")) return 1;
    std::cout << "v1 B+ tree: OK\n";
}