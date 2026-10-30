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

    BTreeLatch tree;
    if (!require(tree.init(8) == RCOK, "init")) return 1;
    constexpr std::size_t kThreads = 4;
    constexpr std::size_t kKeysPerThread = 512;
    std::vector<std::uint64_t> values(kThreads * kKeysPerThread);
    std::atomic<bool> failed{false};
    std::vector<std::thread> writers;
    for (std::size_t t = 0; t < kThreads; ++t) {
        writers.emplace_back([&, t] {
            for (std::size_t i = 0; i < kKeysPerThread; ++i) {
                const std::uint64_t key = static_cast<std::uint64_t>(t * kKeysPerThread + i);
                values[key] = key + 1000;
                if (tree.insert(key, &values[key]) != RCOK) failed = true;
            }
        });
    }
    std::vector<std::thread> readers;
    for (std::size_t t = 0; t < kThreads; ++t) {
        readers.emplace_back([&, t] {
            for (std::size_t i = 0; i < 4000; ++i) {
                const std::uint64_t key = static_cast<std::uint64_t>((i * 97 + t) % (kThreads * kKeysPerThread));
                void* value = nullptr;
                tree.lookup(key, value);
                tree.scan_range(KeyRangeEnt::half_open(key, key + 4));
            }
        });
    }
    for (auto& thread : writers) thread.join();
    for (auto& thread : readers) thread.join();
    if (!require(!failed.load(), "writer result") ||
        !require(tree.size() == kThreads * kKeysPerThread, "size") ||
        !require(tree.validate(), "structure") ||
        !require(tree.height() >= 3, "height")) return 1;

    for (std::uint64_t key : {0ULL, 511ULL, 512ULL, 2047ULL}) {
        void* value = nullptr;
        if (!require(tree.lookup(key, value) && value != nullptr &&
                     *static_cast<std::uint64_t*>(value) == key + 1000, "lookup")) return 1;
    }
    std::cout << "v2 B+ tree latch coupling: OK\n";
}