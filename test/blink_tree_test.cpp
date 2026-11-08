#include "index/btree.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using namespace rrl;

int main() {
    auto require = [](bool condition, const char* message) {
        if (!condition) { std::cerr << "FAIL: " << message << "\n"; return false; }
        return true;
    };

    BTreeBlink tree;
    if (!require(tree.init(8) == RCOK, "init")) return 1;
    std::vector<std::uint64_t> values(4096);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = i + 10000;
        if (!require(tree.insert(i, &values[i]) == RCOK, "insert")) return 1;
    }
    if (!require(tree.size() == values.size(), "size after split") ||
        !require(tree.height() >= 3, "internal split") ||
        !require(tree.validate(), "links after split")) return 1;

    for (std::uint64_t key : {0ULL, 7ULL, 511ULL, 2047ULL, 4095ULL}) {
        void* value = nullptr;
        if (!require(tree.lookup(key, value) && value == &values[key] &&
                     *static_cast<std::uint64_t*>(value) == key + 10000, "lookup")) return 1;
    }
    const auto rows = tree.scan_range(KeyRangeEnt::half_open(123, 987));
    if (!require(rows.size() == 864, "cross leaf scan") ||
        !require(rows.front().key == 123 && rows.back().key == 986, "scan bounds")) return 1;

    constexpr std::size_t kThreads = 4;
    constexpr std::size_t kExtra = 512;
    std::vector<std::uint64_t> extra(kThreads * kExtra);
    std::atomic<bool> failed{false};
    std::vector<std::thread> writers;
    for (std::size_t t = 0; t < kThreads; ++t) {
        writers.emplace_back([&, t] {
            for (std::size_t i = 0; i < kExtra; ++i) {
                const auto key = static_cast<std::uint64_t>(values.size() + t * kExtra + i);
                extra[t * kExtra + i] = key + 10000;
                if (tree.insert(key, &extra[t * kExtra + i]) != RCOK) failed.store(true);
            }
        });
    }
    std::vector<std::thread> readers;
    for (std::size_t t = 0; t < kThreads; ++t) {
        readers.emplace_back([&, t] {
            for (std::size_t i = 0; i < 5000; ++i) {
                const auto key = static_cast<std::uint64_t>((i * 97 + t) % (values.size() + kThreads * kExtra));
                void* value = nullptr;
                tree.lookup(key, value);
                tree.scan_range(KeyRangeEnt::half_open(key, key + 4));
            }
        });
    }
    for (auto& thread : writers) thread.join();
    for (auto& thread : readers) thread.join();
    if (!require(!failed.load(), "concurrent writers") ||
        !require(tree.size() == values.size() + extra.size(), "concurrent size") ||
        !require(tree.validate(), "concurrent links")) return 1;

    for (std::uint64_t key = 0; key < values.size(); key += 3)
        if (!require(tree.erase(key) == RCOK, "erase")) return 1;
    if (!require(tree.validate(), "links after erase") ||
        !require(tree.size() == values.size() + extra.size() - (values.size() + 2) / 3, "size after erase")) return 1;
    for (std::uint64_t key = 0; key < values.size(); ++key) {
        void* value = nullptr;
        const bool found = tree.lookup(key, value);
        if (key % 3 == 0 && !require(!found, "erased lookup")) return 1;
        if (key % 3 != 0 && !require(found, "retained lookup")) return 1;
    }
    std::cout << "B-link tree high-key/right-link: OK\n";
}
