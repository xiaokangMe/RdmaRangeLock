// Is the BTreeLatch update regression a cache-line alignment effect?
//
// The update-only microbenchmark in benchmarks/latch_path_bench.cpp measured
// ~20% slower after the write path was rewritten, while the same code compiled
// into another translation unit was not. That points at placement rather than
// at the algorithm, so this benchmark isolates the one placement question a page
// layout can actually change: whether the latch's state word shares a cache line
// with other latches, or with the data the latch protects.
//
// Three layouts over an identical access pattern (two-level shared descent, then
// an exclusive leaf latch and one value write):
//
//   dense    - every latch lives in one contiguous array, so neighbouring pages'
//              latches share cache lines. This is the positive control: if the
//              harness cannot separate it from the others it cannot detect a
//              placement effect at all, and a null result elsewhere is noise.
//   page     - the layout BTreePage uses: keys, values and a pointer first, the
//              latch last, one page object per allocation.
//   isolated - the latch owns a whole cache line and the data starts on the next.
//
// Note what `page` already implies: because the latch is the *last* member, it is
// followed by allocator padding, and the keys/values are heap buffers pointed to
// by vectors, so a page's latch line holds no contended data. The comparison of
// interest is therefore `page` vs `isolated`; `dense` says whether the harness
// is capable of showing a difference at all.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kLine = 64;
constexpr std::size_t kKeysPerPage = 16;
constexpr std::size_t kLevel1Pages = 64;

// One page worth of user data, identical in every layout.
struct PageData {
    std::uint64_t keys[kKeysPerPage];
    std::uint64_t values[kKeysPerPage];
    void* next{nullptr};
};

// Latches packed into their own array: consecutive latches share a cache line.
struct DenseStorage {
    std::unique_ptr<std::shared_mutex[]> latches;
    std::unique_ptr<PageData[]> pages;

    explicit DenseStorage(std::size_t pages)
        : latches(new std::shared_mutex[pages]), pages(new PageData[pages]) {}

    std::shared_mutex& latch(std::size_t index) { return latches[index]; }
    PageData& data(std::size_t index) { return pages[index]; }
};

// The layout BTreePage uses: data first, latch last, one object per page.
struct PageStorage {
    struct Slot {
        PageData data;
        std::shared_mutex latch;
    };
    std::unique_ptr<Slot[]> slots;

    explicit PageStorage(std::size_t pages) : slots(new Slot[pages]) {}

    std::shared_mutex& latch(std::size_t index) { return slots[index].latch; }
    PageData& data(std::size_t index) { return slots[index].data; }
};

// The latch gets a whole line, the data starts on the next one.
struct alignas(kLine) IsolatedSlot {
    std::shared_mutex latch;
    char pad[kLine - sizeof(std::shared_mutex)]{};
    PageData data;
};

struct IsolatedStorage {
    std::unique_ptr<IsolatedSlot[]> slots;

    explicit IsolatedStorage(std::size_t pages) : slots(new IsolatedSlot[pages]) {}

    std::shared_mutex& latch(std::size_t index) { return slots[index].latch; }
    PageData& data(std::size_t index) { return slots[index].data; }
};

static_assert(sizeof(IsolatedSlot) > kLine, "isolated layout must separate latch from data");
static_assert(sizeof(PageStorage::Slot) > kLine, "page layout is expected to span more than a line");
static_assert(sizeof(std::shared_mutex) < kLine, "latch must leave room inside one line");

std::uint64_t splitmix64(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

template <typename Storage>
double run_layout(std::size_t threads, std::size_t leaf_pages, std::size_t ops_per_thread,
                  bool interleave) {
    Storage leaves(leaf_pages);
    PageStorage level1(kLevel1Pages);
    std::shared_mutex root_latch;
    std::uint64_t root_keys[kLevel1Pages];
    for (std::size_t i = 0; i < kLevel1Pages; ++i) root_keys[i] = i;
    for (std::size_t i = 0; i < leaf_pages; ++i) {
        for (std::size_t k = 0; k < kKeysPerPage; ++k) {
            leaves.data(i).keys[k] = i * kKeysPerPage + k;
            leaves.data(i).values[k] = i;
        }
    }

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (std::size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            std::uint64_t state = 0x1234u + t * 7919u;
            // Disjoint: each thread owns a contiguous range, which is what
            // benchmarks/latch_path_bench.cpp does. Interleaved: neighbouring
            // pages belong to different threads, which is the only pattern in
            // which latches that share a cache line are also written by
            // different cores. Without the second mode a dense-latch layout
            // cannot show a false-sharing penalty at all.
            const std::size_t span = std::max<std::size_t>(1, leaf_pages / threads);
            const std::size_t low = std::min(t * span, leaf_pages - std::min(span, leaf_pages));
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (std::size_t i = 0; i < ops_per_thread; ++i) {
                std::size_t leaf = low + splitmix64(state) % span;
                if (interleave) leaf = (splitmix64(state) % span) * threads + t;
                if (leaf >= leaf_pages) leaf = leaf % leaf_pages;
                const std::size_t slot_index = splitmix64(state) % kKeysPerPage;
                {
                    // Two-level shared descent, as insert_new does.
                    std::shared_lock<std::shared_mutex> root(root_latch);
                    const std::size_t branch = leaf % kLevel1Pages;
                    std::shared_lock<std::shared_mutex> parent(level1.latch(branch));
                    (void)root_keys[branch];
                }
                // Leaf upgrade, then the write the latch protects.
                std::unique_lock<std::shared_mutex> guard(leaves.latch(leaf));
                leaves.data(leaf).values[slot_index] = leaves.data(leaf).keys[slot_index];
            }
        });
    }
    const auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    return static_cast<double>(threads * ops_per_thread) / seconds;
}

struct Options {
    std::string variant{"page"};
    std::size_t threads{1};
    std::size_t pages{8192};
    std::size_t ops{200000};
    bool interleave{false};
};

bool parse(int argc, char** argv, Options& opt) {
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string flag = argv[i];
        const char* value = argv[i + 1];
        if (flag == "--variant") opt.variant = value;
        else if (flag == "--threads") opt.threads = std::strtoull(value, nullptr, 10);
        else if (flag == "--pages") opt.pages = std::strtoull(value, nullptr, 10);
        else if (flag == "--ops") opt.ops = std::strtoull(value, nullptr, 10);
        else if (flag == "--interleave") {
            opt.interleave = std::string(value) == "1";
            continue;
        }
        else {
            std::fprintf(stderr, "unknown option: %s\n", flag.c_str());
            return false;
        }
    }
    if (opt.threads == 0 || opt.pages < 64 || opt.ops == 0) return false;
    return opt.variant == "dense" || opt.variant == "page" || opt.variant == "isolated";
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) {
        std::fprintf(stderr,
                     "usage: %s --variant dense|page|isolated --threads N --pages N --ops N\n",
                     argv[0]);
        return 2;
    }
    const double rate =
        opt.variant == "dense"
            ? run_layout<DenseStorage>(opt.threads, opt.pages, opt.ops, opt.interleave)
        : opt.variant == "page"
            ? run_layout<PageStorage>(opt.threads, opt.pages, opt.ops, opt.interleave)
            : run_layout<IsolatedStorage>(opt.threads, opt.pages, opt.ops, opt.interleave);
    std::printf("variant=%s threads=%zu pages=%zu interleave=%d ops_per_sec=%.0f\n",
                opt.variant.c_str(), opt.threads, opt.pages, int(opt.interleave), rate);
    return 0;
}
