// Dedicated micro-benchmark for the two BTreeLatch write paths.
//
//   update-only : the tree is pre-filled, every operation updates an existing key.
//                 This is the path optimised by `update_existing()` (leaf latch only),
//                 so it must not touch `writer_guard_`.
//   insert-only : every operation inserts a key that does not exist yet, so it always
//                 takes the writer/root-split path and forces leaf and root splits.
//
// Threads work on disjoint key ranges on purpose: the point is to isolate structural
// contention (root_guard_ / writer_guard_) from logical contention on the same key.
// Everything before the timed region is setup and must not be counted as throughput.

#include "index/btree.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <algorithm>
#include <thread>
#include <vector>

using namespace rrl;

namespace {

constexpr std::uint64_t kGolden = 0x9e3779b97f4a7c15ULL;

std::uint64_t splitmix64(std::uint64_t& state) {
    std::uint64_t z = (state += kGolden);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

struct Options {
    std::string mode{"update"};
    std::string build{"local"};
    std::size_t order{128};
    std::size_t threads{1};
    std::size_t keys{65536};
    std::size_t ops_per_thread{200000};
    std::uint64_t seed{8};
    std::size_t run{1};
    bool csv{false};
};

struct Counter {
    std::atomic<std::uint64_t> ops{0};
    std::atomic<std::uint64_t> committed{0};
    std::atomic<std::uint64_t> failed{0};
};

void usage() {
    std::cout <<
        "usage: latch_path_bench --mode update|insert --order N --threads N\n"
        "                        --keys N (prefill size) --ops N (per thread)\n"
        "                        --seed N --run N --build LABEL [--format csv]\n";
}

bool parse(Options& opt, int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--help" || flag == "-h") { usage(); return false; }
        if (flag == "--format") {
            if (i + 1 < argc && std::strcmp(argv[++i], "csv") == 0) opt.csv = true;
            continue;
        }
        if (i + 1 >= argc) { usage(); return false; }
        const std::string value = argv[++i];
        if (flag == "--mode") opt.mode = value;
        else if (flag == "--build") opt.build = value;
        else if (flag == "--order") opt.order = std::stoull(value);
        else if (flag == "--threads") opt.threads = std::stoull(value);
        else if (flag == "--keys") opt.keys = std::stoull(value);
        else if (flag == "--ops") opt.ops_per_thread = std::stoull(value);
        else if (flag == "--seed") opt.seed = std::stoull(value);
        else if (flag == "--run") opt.run = std::stoull(value);
        else { std::cerr << "unknown option: " << flag << "\n"; return false; }
    }
    if (opt.mode != "update" && opt.mode != "insert") {
        std::cerr << "--mode must be update or insert\n";
        return false;
    }
    if (opt.threads == 0 || opt.order < 3 || opt.keys == 0) {
        std::cerr << "invalid matrix parameters\n";
        return false;
    }
    return true;
}

// Pre-fills [0, keys) single threaded. Not part of the measured region.
bool prefill(BTreeLatch& tree, std::size_t order, std::size_t keys,
             std::vector<std::uint64_t>& values) {
    if (tree.init(order) != RCOK) return false;
    for (std::size_t i = 0; i < keys; ++i) {
        values[i] = i;
        if (tree.insert(static_cast<idx_key_t>(i), &values[i]) != RCOK) return false;
    }
    return true;
}

void run_update(BTreeLatch& tree, const Options& opt, std::size_t thread,
                std::vector<std::uint64_t>& values, Counter& counter) {
    const std::size_t span = std::max<std::size_t>(1, opt.keys / opt.threads);
    const std::size_t low = std::min(thread * span, opt.keys - std::min(span, opt.keys));
    std::uint64_t state = opt.seed * kGolden + thread + 1;
    std::uint64_t local_ok = 0;
    std::uint64_t local_fail = 0;
    for (std::size_t i = 0; i < opt.ops_per_thread; ++i) {
        const idx_key_t key = static_cast<idx_key_t>(low + splitmix64(state) % span);
        if (tree.insert(key, &values[key]) == RCOK) ++local_ok;
        else ++local_fail;
    }
    counter.ops.fetch_add(opt.ops_per_thread);
    counter.committed.fetch_add(local_ok);
    counter.failed.fetch_add(local_fail);
}

void run_insert(BTreeLatch& tree, const Options& opt, std::size_t thread,
                std::vector<std::uint64_t>& values, Counter& counter) {
    // Disjoint, ascending, never-reused key blocks: every insert is a new key, and
    // each thread keeps growing its own region so leaf and root splits must happen.
    const std::size_t base = opt.keys + thread * opt.ops_per_thread;
    std::uint64_t local_ok = 0;
    std::uint64_t local_fail = 0;
    for (std::size_t i = 0; i < opt.ops_per_thread; ++i) {
        const std::size_t slot = base + i;
        const idx_key_t key = static_cast<idx_key_t>(base + i);
        if (tree.insert(key, &values[slot]) == RCOK) ++local_ok;
        else ++local_fail;
    }
    counter.ops.fetch_add(opt.ops_per_thread);
    counter.committed.fetch_add(local_ok);
    counter.failed.fetch_add(local_fail);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(opt, argc, argv)) return 2;

    // A fresh process and a fresh tree per case: nothing is reused across runs.
    BTreeLatch tree;
    // Size the value storage up front: the tree stores raw pointers into it, so it
    // must never reallocate after prefill starts.
    const std::size_t value_slots =
        opt.keys + 64 + (opt.mode == "insert" ? opt.threads * opt.ops_per_thread : 0);
    std::vector<std::uint64_t> values(value_slots);
    if (!prefill(tree, opt.order, opt.keys, values)) {
        std::cerr << "prefill failed\n";
        return 2;
    }
    const std::size_t size_before = tree.size();
    const std::size_t height_before = tree.height();
    if (!tree.validate()) {
        std::cerr << "prefill produced an invalid tree\n";
        return 2;
    }

    Counter counter;
    std::vector<std::thread> workers;
    workers.reserve(opt.threads);
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t t = 0; t < opt.threads; ++t) {
        workers.emplace_back(opt.mode == "update" ? run_update : run_insert,
                             std::ref(tree), std::cref(opt), t, std::ref(values),
                             std::ref(counter));
    }
    for (auto& worker : workers) worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    // Everything below runs on the quiescent tree.
    const std::size_t size_after = tree.size();
    const std::size_t height_after = tree.height();
    const bool valid = tree.validate();
    const auto elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    const std::uint64_t ops = counter.ops.load();
    const double ops_per_sec = elapsed_ns == 0 ? 0.0
        : static_cast<double>(ops) * 1e9 / static_cast<double>(elapsed_ns);

    if (opt.csv) {
        std::cout << "build,mode,order,threads,run,seed,prefill_keys,ops_per_thread,"
                     "ops_total,committed,failed,elapsed_ns,ops_per_sec,"
                     "size_before,height_before,size_after,height_after,validate_ok\n";
        std::cout << opt.build << ',' << opt.mode << ',' << opt.order << ',' << opt.threads << ','
                  << opt.run << ',' << opt.seed << ',' << opt.keys << ',' << opt.ops_per_thread << ','
                  << ops << ',' << counter.committed.load() << ',' << counter.failed.load() << ','
                  << elapsed_ns << ',' << ops_per_sec << ',' << size_before << ',' << height_before
                  << ',' << size_after << ',' << height_after << ',' << (valid ? 1 : 0) << '\n';
    } else {
        std::cout << "build=" << opt.build << " mode=" << opt.mode
                  << " threads=" << opt.threads << " run=" << opt.run << " seed=" << opt.seed
                  << " ops=" << ops << " committed=" << counter.committed.load()
                  << " failed=" << counter.failed.load()
                  << " ops_per_sec=" << ops_per_sec
                  << " size=" << size_before << "->" << size_after
                  << " height=" << height_before << "->" << height_after
                  << " validate=" << (valid ? "OK" : "FAIL") << '\n';
    }
    return valid ? 0 : 3;
}
