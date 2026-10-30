#include "index/btree.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

using namespace rrl;

namespace {

std::size_t parse_size(const char* text, std::size_t fallback) {
    if (!text || *text == '\0') return fallback;
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    return end && *end == '\0' && value > 0 ? static_cast<std::size_t>(value) : fallback;
}

bool check_scan(const std::vector<KeyValue>& rows, const KeyRangeEnt& range,
                const std::vector<std::uint64_t>& values) {
    idx_key_t previous = 0;
    bool first = true;
    for (const auto& row : rows) {
        if (row.key >= values.size() || !range.contains(row.key) || row.value == nullptr ||
            *static_cast<const std::uint64_t*>(row.value) != row.key + 1000000) return false;
        if (!first && row.key <= previous) return false;
        previous = row.key;
        first = false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const auto hardware = std::max<unsigned>(1, std::thread::hardware_concurrency());
    const std::size_t threads = parse_size(argc > 1 ? argv[1] : nullptr,
                                           std::min<std::size_t>(hardware, 64));
    const std::size_t seconds = parse_size(argc > 2 ? argv[2] : nullptr, 3);
    const std::size_t keys_per_thread = parse_size(argc > 3 ? argv[3] : nullptr, 2048);
    const std::size_t total_keys = threads * keys_per_thread;

    BTreeLatch tree;
    if (tree.init(8) != RCOK) {
        std::cerr << "FAIL: init\n";
        return 1;
    }
    std::vector<std::uint64_t> values(total_keys);
    for (std::size_t key = 0; key < total_keys; ++key) values[key] = key + 1000000;

    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::atomic<std::uint64_t> operations{0};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (std::size_t worker = 0; worker < threads; ++worker) {
        workers.emplace_back([&, worker] {
            std::mt19937_64 random(0x9e3779b97f4a7c15ULL + worker);
            const std::size_t begin = worker * keys_per_thread;
            std::size_t cursor = begin;
            while (!stop.load(std::memory_order_relaxed)) {
                const std::size_t key = begin + (cursor++ - begin) % keys_per_thread;
                const auto action = random() % 4;
                if (action == 0 || action == 1) {
                    if (tree.insert(key, &values[key]) != RCOK) failed.store(true);
                } else if (action == 2) {
                    void* value = nullptr;
                    if (tree.lookup(random() % total_keys, value) &&
                        (value == nullptr || *static_cast<std::uint64_t*>(value) < 1000000))
                        failed.store(true);
                } else {
                    const std::size_t left = random() % total_keys;
                    const std::size_t width = 1 + random() % 32;
                    const auto right = std::min(total_keys, left + width);
                    const auto rows = tree.scan_range(KeyRangeEnt::half_open(left, right));
                    if (!check_scan(rows, KeyRangeEnt::half_open(left, right), values))
                        failed.store(true);
                }
                ++operations;
                if ((operations.load(std::memory_order_relaxed) & 0x3ff) == 0 &&
                    std::chrono::steady_clock::now() >= deadline) stop.store(true);
            }
        });
    }
    for (auto& worker : workers) worker.join();

    for (std::size_t key = 0; key < total_keys; ++key)
        if (tree.insert(key, &values[key]) != RCOK) failed.store(true);

    if (failed.load() || tree.size() != total_keys || !tree.validate()) {
        std::cerr << "FAIL: threads=" << threads << " seconds=" << seconds
                  << " keys=" << total_keys << " operations=" << operations.load()
                  << " size=" << tree.size() << " valid=" << tree.validate() << "\n";
        return 1;
    }
    std::cout << "v2 B+ tree stress: OK threads=" << threads
              << " seconds=" << seconds << " keys=" << total_keys
              << " operations=" << operations.load() << " height=" << tree.height() << "\n";
    return 0;
}
