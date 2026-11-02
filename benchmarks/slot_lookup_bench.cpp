// Measures one complete snapshot lookup: build + all conflicts + exact owner.
#include "lock/slot_index.h"
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
using namespace rrl;
int main(int argc, char** argv) {
    if (argc != 6) return 2;
    const auto size = std::stoull(argv[1]), iterations = std::stoull(argv[2]);
    const std::string shape = argv[3], policy = argv[4];
    if (size > kRemoteLockSlotsPerShard || !iterations ||
        (policy != "index" && policy != "linear")) return 2;
    std::mt19937_64 random(std::stoull(argv[5]));
    std::vector<RemoteLockSlot> slots;
    for (std::size_t i = 0; i < size; ++i) {
        KeyRangeEnt range;
        const auto start = (random() % 16384) * 4;
        if (shape == "points") range = KeyRangeEnt::point(start);
        else if (shape == "wide") range = KeyRangeEnt::closed(start, start + 8192);
        else if (shape == "nested") range = KeyRangeEnt::closed(i, 65536 - i);
        else if (shape == "unbounded") range = KeyRangeEnt::unbounded();
        else if (shape == "mixed") {
            range = i % 3 == 0 ? KeyRangeEnt::unbounded() :
                KeyRangeEnt::half_open(start, start + 1 + random() % 256);
        } else return 2;
        RemoteLockSlot slot{};
        encode_remote_slot(slot, 1, 1 + i % 32, LockMode::S, LockKind::NextKey, range);
        slots.push_back(slot);
    }
    std::shuffle(slots.begin(), slots.end(), random);
    std::vector<RangeLock> queries;
    for (std::size_t i = 0; i < 256; ++i) {
        if (i % 4 == 0 && !slots.empty()) queries.push_back(decode_remote_slot(slots[i % slots.size()]));
        else queries.push_back({99, i % 2 ? LockMode::X : LockMode::S, LockKind::NextKey,
                                KeyRangeEnt::point(random() % 65536)});
    }
    SlotIndex lookup;
    lookup.reserve(kRemoteLockSlotsPerShard);
    auto run = [&](std::size_t n) {
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const auto& request = queries[i % queries.size()];
            lookup.build(slots, policy == "linear");
            lookup.for_each_candidate(slots, request.range, [&](const RemoteLockSlot& slot) {
                sum += request.conflicts(decode_remote_slot(slot));
            });
            std::size_t owned = 0;
            sum += lookup.find_owned(slots, request.txn_id, request.kind, request.range, owned);
        }
        return sum;
    };
    const auto warmup = run(16);
    const auto start = std::chrono::steady_clock::now();
    const auto sum = run(iterations);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "slots,shape,policy,iterations,total_ns,ns_per_lookup,checksum,warmup\n"
              << size << ',' << shape << ',' << policy << ',' << iterations << ','
              << ns << ',' << static_cast<double>(ns) / iterations << ',' << sum << ',' << warmup << '\n';
}
