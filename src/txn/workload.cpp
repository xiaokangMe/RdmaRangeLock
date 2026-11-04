#include "txn/workload.h"
#include "hierarchical_plan.h"
#include <iostream>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace rrl {
namespace {

struct Zipf {
    std::vector<double> c;
    explicit Zipf(size_t n, double theta) : c(n) {
        double z = 0;
        for (size_t i = 1; i <= n; i++) z += 1.0 / std::pow(double(i), theta);
        double s = 0;
        for (size_t i = 1; i <= n; i++) {
            s += (1.0 / std::pow(double(i), theta)) / z;
            c[i - 1] = s;
        }
        if (!c.empty()) c.back() = 1;
    }
    idx_key_t get(std::mt19937_64& g) const {
        if (c.empty()) return 0;
        double x = std::generate_canonical<double, 53>(g);
        return idx_key_t(std::lower_bound(c.begin(), c.end(), x) - c.begin());
    }
};

idx_key_t key(std::mt19937_64& g, const WorkloadConfig& c, const Zipf& z) {
    if (!c.key_count) return 0;
    return c.zipf_theta > 0 ? z.get(g)
                            : std::uniform_int_distribution<idx_key_t>(0, c.key_count - 1)(g);
}

void ycsb(TransactionTrace& t, const WorkloadConfig& c, std::mt19937_64& g, const Zipf& z,
          size_t n, size_t& mx) {
    double rr = c.kind == WorkloadKind::YcsbA ? .5 : .95;
    std::bernoulli_distribution read(rr), ins(.05);
    // Deneva ordinary YCSB generation: reject duplicate request keys and
    // replenish to REQ_PER_QUERY. KEY_ORDER=false preserves generation order.
    // Ranges may overlap; uniqueness applies to request anchors, not all rows.
    std::unordered_set<idx_key_t> selected;
    t.operations.reserve(c.operations_per_transaction);
    size_t rejected = 0;
    for (size_t i = 0; i < c.operations_per_transaction;) {
        idx_key_t k = key(g, c, z);
        if (c.ycsb_unique_keys && !selected.insert(k).second) {
            // Diagnose unsupported extreme skew rather than hanging forever.
            if (++rejected >= 1048576)
                throw std::invalid_argument("YCSB unique-key sampling exhausted; reduce skew or request count");
            continue;
        }
        Operation o;
        o.value = (uint64_t(n) << 32) | i;
        if (c.kind == WorkloadKind::YcsbE && !ins(g)) {
            o.kind = OperationKind::RangeRead;
            o.key = k;
            o.end_key = std::min<idx_key_t>(c.key_count, k + c.range_length);
            mx = std::max(mx, size_t(o.end_key));
        } else if (c.kind == WorkloadKind::YcsbE) {
            o.kind = OperationKind::Insert;
            // Historical repeated-key fixtures retain their exact generator.
            o.key = c.ycsb_unique_keys ? k : (c.key_count == 0 ? 0 : std::min<idx_key_t>(
                c.key_count - 1,
                k + std::max<size_t>(1, c.range_length / 2) +
                    (i % std::max<size_t>(1, c.range_length))));
        } else {
            o.kind = read(g) ? OperationKind::Read : OperationKind::Update;
            o.key = k;
        }
        mx = std::max(mx, size_t(o.key));
        t.operations.push_back(o);
        ++i;
    }
}

void payment(TransactionTrace& t, const WorkloadConfig& c, size_t n, size_t& mx) {
    idx_key_t w = n % std::max<size_t>(1, c.key_count), d = 1000000 + w * 10 + n % 10,
              u = 2000000 + w * 30000 + n % 30000;
    t.operations = {{OperationKind::Update, w, w, n + 1},
                    {OperationKind::Update, d, d, n + 2},
                    {OperationKind::Update, u, u, n + 3}};
    mx = std::max(mx, size_t(u));
}

void new_order(TransactionTrace& t, const WorkloadConfig& c, std::mt19937_64& g, size_t n,
               size_t& mx) {
    idx_key_t w = n % std::max<size_t>(1, c.key_count), d = 1000000 + w * 10 + n % 10,
              u = 2000000 + w * 30000 + n % 30000;
    t.operations = {{OperationKind::Read, w, w, n + 1},
                    {OperationKind::Read, u, u, n + 2},
                    {OperationKind::Update, d, d, n + 3}};
    std::uniform_int_distribution<idx_key_t> item(0, std::max<size_t>(1, c.key_count) - 1);
    for (size_t i = 0; i < c.tpcc_items_per_order; i++) {
        idx_key_t ik = 3000000 + item(g), sk = 4000000 + w * std::max<size_t>(1, c.key_count) + item(g);
        t.operations.push_back({OperationKind::Read, ik, ik, n + i + 4});
        t.operations.push_back({OperationKind::Update, sk, sk, n + i + 5});
        mx = std::max(mx, size_t(sk));
    }
    mx = std::max(mx, size_t(u));
}

}  // namespace

using SeedValues = std::unordered_map<idx_key_t, std::uint64_t>;

bool seed_index(BTreeIndex& index, const WorkloadTrace& trace, SeedValues& seed) {
    std::unordered_set<idx_key_t> insert_keys;
    for (const auto& txn : trace.transactions())
        for (const auto& op : txn.operations)
            if (op.kind == OperationKind::Insert) insert_keys.insert(op.key);
    for (size_t k = 0; k < trace.config().key_count; ++k) {
        if (insert_keys.find(k) != insert_keys.end()) continue;
        auto [it, inserted] = seed.emplace(k, k);
        (void)inserted;
        if (index.insert(k, &it->second) != RCOK) return false;
    }
    for (const auto& txn : trace.transactions())
        for (const auto& op : txn.operations) {
            if (op.kind == OperationKind::Insert || op.kind == OperationKind::Delete) continue;
            auto [it, inserted] = seed.emplace(op.key, op.key);
            if (inserted && index.insert(op.key, &it->second) != RCOK) return false;
        }
    return true;
}

namespace {

// Statistics are accumulated per thread and merged once at the end. Every
// counter used to be a std::atomic incremented per operation, which put a
// shared cache line on the critical path of exactly the runs whose scaling was
// being measured.
// Cache-line aligned and padded so neighbouring threads never write into the
// same line; without this the per-thread counters reintroduce false sharing.
struct alignas(64) LocalAcc {
    std::uint64_t tx{0}, committed{0}, aborted{0}, ops{0};
    std::uint64_t reads{0}, updates{0}, inserts{0}, deletes{0};
    std::uint64_t ranges{0}, found{0}, scanned{0}, rechecked{0};
    std::uint64_t acquires{0}, conflicts{0}, capacity{0}, gate{0};
    std::uint64_t checksum{0};
    std::uint64_t lock_errors{0}, index_errors{0}, release_errors{0}, failed_tx{0};
    std::uint64_t held{0};
};
static_assert(sizeof(LocalAcc) % 64 == 0, "per-thread counters must not share a cache line");

RunStats merge_parts(const std::vector<LocalAcc>& parts,
                     std::chrono::steady_clock::time_point begin) {
    RunStats result;
    for (const auto& part : parts) {
        result.transactions += part.tx;
        result.committed += part.committed;
        result.aborted += part.aborted;
        result.operations += part.ops;
        result.reads += part.reads;
        result.updates += part.updates;
        result.inserts += part.inserts;
        result.deletes += part.deletes;
        result.range_reads += part.ranges;
        result.rows_found += part.found;
        result.rows_scanned += part.scanned;
        result.rows_rechecked += part.rechecked;
        result.lock_acquires += part.acquires;
        result.lock_conflicts += part.conflicts;
        result.lock_capacity_aborts += part.capacity;
        result.lock_gate_aborts += part.gate;
        result.checksum += part.checksum;
        result.lock_errors += part.lock_errors;
        result.index_errors += part.index_errors;
        result.lock_release_failures += part.release_errors;
        result.failed_transactions += part.failed_tx;
    }
    result.elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin).count());
    return result;
}

KeyRangeEnt right_closed(idx_key_t left, idx_key_t right) {
    KeyRangeEnt range;
    range.StartKey = left; range.EndKey = right;
    range.has_start = true; range.has_end = true;
    range.start_bound = Bound::Open; range.end_bound = Bound::Closed;
    return range;
}

}  // namespace

WorkloadKind WorkloadTrace::parse_kind(const std::string& in) {
    std::string v;
    for (char x : in) v += char(std::tolower((unsigned char)x));
    if (v == "a" || v == "ycsb-a") return WorkloadKind::YcsbA;
    if (v == "b" || v == "ycsb-b") return WorkloadKind::YcsbB;
    if (v == "e" || v == "ycsb-e") return WorkloadKind::YcsbE;
    if (v == "payment" || v == "tpcc-payment") return WorkloadKind::TpccPayment;
    if (v == "new-order" || v == "tpcc-new-order") return WorkloadKind::TpccNewOrder;
    throw std::invalid_argument("unknown workload: " + in);
}

const char* WorkloadTrace::name(WorkloadKind k) {
    switch (k) {
        case WorkloadKind::YcsbA: return "ycsb-a";
        case WorkloadKind::YcsbB: return "ycsb-b";
        case WorkloadKind::YcsbE: return "ycsb-e";
        case WorkloadKind::TpccPayment: return "tpcc-payment";
        case WorkloadKind::TpccNewOrder: return "tpcc-new-order";
    }
    return "unknown";
}

WorkloadTrace WorkloadTrace::generate(const WorkloadConfig& c) {
    const bool ycsb_kind = c.kind == WorkloadKind::YcsbA || c.kind == WorkloadKind::YcsbB || c.kind == WorkloadKind::YcsbE;
    if (ycsb_kind && c.ycsb_unique_keys && (!c.key_count || c.operations_per_transaction > c.key_count))
        throw std::invalid_argument("YCSB unique requests must not exceed key_count");
    WorkloadTrace t;
    t.config_ = c;
    std::mt19937_64 g(c.seed);
    Zipf z(c.key_count, c.zipf_theta);
    t.transactions_.reserve(c.transaction_count);
    for (size_t i = 0; i < c.transaction_count; i++) {
        TransactionTrace x;
        x.txn_id = i + 1;
        if (c.kind == WorkloadKind::TpccPayment) payment(x, c, i, t.max_key_);
        else if (c.kind == WorkloadKind::TpccNewOrder) new_order(x, c, g, i, t.max_key_);
        else ycsb(x, c, g, z, i, t.max_key_);
        t.transactions_.push_back(std::move(x));
    }
    return t;
}

double RunStats::throughput() const {
    return elapsed_ns ? double(committed) * 1e9 / double(elapsed_ns) : 0;
}

double RunStats::attempt_throughput() const {
    return elapsed_ns ? double(transactions) * 1e9 / double(elapsed_ns) : 0;
}

namespace {

template <typename Acc>
bool record_lock_result(Acc& acc, std::atomic<bool>& stopped, RC rc) {
    if (rc == RCOK) { ++acc.held; return true; }
    if (rc == AbortCapacity) ++acc.capacity;
    else if (rc == AbortGate) ++acc.gate;
    else if (rc == AbortConflict) ++acc.conflicts;
    else { ++acc.lock_errors; stopped.store(true, std::memory_order_relaxed); }
    return false;
}

// A write error invalidates the run: there is no undo log in this harness.
// Do not report a partially applied write set as a committed transaction.
bool apply_write(BTreeIndex& index, const Operation& op, LocalAcc& acc,
                 std::atomic<bool>& stopped) {
    const RC rc = op.kind == OperationKind::Delete ? index.erase(op.key)
        : index.insert(op.key, const_cast<uint64_t*>(&op.value));
    // Delete of a missing key is a workload no-op; other non-RCOK values fail.
    if (rc == RCOK || (op.kind == OperationKind::Delete && rc == Abort)) return true;
    ++acc.index_errors;
    stopped.store(true, std::memory_order_relaxed);
    return false;
}

void finish_transaction(BTreeIndex& index, IRangeLockManager& locks, txnid_t tid,
                        const std::vector<const Operation*>& writes, bool ok,
                        LocalAcc& acc, std::atomic<bool>& stopped,
                        std::uint64_t* release_ns = nullptr) {
    if (ok) for (const auto* op : writes)
        if (!apply_write(index, *op, acc, stopped)) { ok = false; break; }
    // The commit/abort decision precedes strict-2PL release. A release failure
    // invalidates measurement even when the writes were already committed.
    if (acc.held || acc.lock_errors) {
        const auto release_start = release_ns ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const RC rc = locks.unlock_all(tid);
        if (release_ns) *release_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - release_start).count());
        if (rc != RCOK && !(acc.held == 0 && rc == Abort)) {
            ++acc.release_errors;
            stopped.store(true, std::memory_order_relaxed);
        }
    }
    if (acc.lock_errors || acc.index_errors || acc.release_errors) ++acc.failed_tx;
    else if (ok) ++acc.committed;
    else ++acc.aborted;
    ++acc.tx;
}

}  // namespace

RunStats run_workload(BTreeIndex& index, const WorkloadTrace& trace, size_t threads) {
    if (!threads) threads = 1;
    SeedValues seed;
    if (!seed_index(index, trace, seed)) { RunStats failed; failed.index_errors = 1; return failed; }
    if (trace.transactions().empty()) return {};
    std::atomic<bool> stopped{false};
    const size_t worker_count = std::min(threads, std::max<size_t>(1, trace.transactions().size()));
    std::vector<LocalAcc> parts(worker_count);
    const auto begin = std::chrono::steady_clock::now();
    const size_t total_txns = trace.transactions().size();
    const bool timed = trace.config().duration_seconds != 0;
    const auto deadline = timed ? std::chrono::steady_clock::now() +
        std::chrono::seconds(trace.config().duration_seconds)
        : std::chrono::steady_clock::time_point::max();
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < worker_count; ++worker)
        workers.emplace_back([&, worker] {
            LocalAcc& a = parts[worker];
            for (size_t j = worker, rounds = 0;; j += worker_count, ++rounds) {
                if (stopped.load(std::memory_order_relaxed)) break;
                a.held = 0;
                if (timed) { if ((rounds % 64) == 0 && std::chrono::steady_clock::now() >= deadline) break; }
                else if (j >= total_txns) break;
                bool ok = true;
                for (const Operation& op : trace.transactions()[j % total_txns].operations) {
                    ++a.ops;
                    switch (op.kind) {
                    case OperationKind::Read: {
                        ++a.reads;
                        void* p = nullptr;
                        if (index.lookup(op.key, p)) {
                            ++a.found;
                            a.checksum += op.key ^ *static_cast<uint64_t*>(p);
                        }
                        break;
                    }
                    case OperationKind::Update:
                        ++a.updates;
                        ok = apply_write(index, op, a, stopped);
                        break;
                    case OperationKind::Insert:
                        ++a.inserts;
                        ok = apply_write(index, op, a, stopped);
                        break;
                    case OperationKind::Delete:
                        ++a.deletes;
                        ok = apply_write(index, op, a, stopped);
                        break;
                    case OperationKind::RangeRead: {
                        ++a.ranges;
                        auto out = index.scan_range(KeyRangeEnt::half_open(op.key, op.end_key));
                        a.scanned += out.size();
                        a.found += out.size();
                        for (auto& v : out) a.checksum += v.key ^ *static_cast<uint64_t*>(v.value);
                        break;
                    }
                    }
                    if (!ok) break;
                }
                ++a.tx;
                if (ok) ++a.committed;
                else ++a.failed_tx;
            }
        });
    for (auto& worker : workers) worker.join();
    return merge_parts(parts, begin);
}

RunStats run_record_2pl(BTreeIndex& index, LockManager& locks, const WorkloadTrace& trace,
                        size_t threads) {
    if (!threads) threads = 1;
    SeedValues seed;
    if (!seed_index(index, trace, seed)) { RunStats failed; failed.index_errors = 1; return failed; }
    if (trace.transactions().empty()) return {};
    std::atomic<bool> stopped{false};
    const size_t worker_count = std::min(threads, std::max<size_t>(1, trace.transactions().size()));
    std::vector<LocalAcc> parts(worker_count);
    const auto begin = std::chrono::steady_clock::now();
    const size_t total_txns = trace.transactions().size();
    const bool timed = trace.config().duration_seconds != 0;
    const auto deadline = timed ? std::chrono::steady_clock::now() +
        std::chrono::seconds(trace.config().duration_seconds)
        : std::chrono::steady_clock::time_point::max();
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < worker_count; ++worker)
        workers.emplace_back([&, worker] {
            LocalAcc& a = parts[worker];
            for (size_t i = worker, rounds = 0;; i += worker_count, ++rounds) {
                if (stopped.load(std::memory_order_relaxed)) break;
                a.held = 0;
                if (timed) { if ((rounds % 64) == 0 && std::chrono::steady_clock::now() >= deadline) break; }
                else if (i >= total_txns) break;
                const auto& txn = trace.transactions()[i % total_txns];
                const txnid_t tid = txn.txn_id +
                    static_cast<txnid_t>(i / total_txns) * static_cast<txnid_t>(total_txns);
                bool ok = true;
                bool stop_after_txn = false;
                std::vector<const Operation*> writes;
                for (const Operation& op : txn.operations) {
                    if (timed && std::chrono::steady_clock::now() >= deadline) { stop_after_txn = true; ok = false; break; }
                    ++a.ops;
                    if (op.kind == OperationKind::RangeRead) {
                        ++a.ranges;
                        const auto rows = index.scan_range(KeyRangeEnt::half_open(op.key, op.end_key));
                        a.scanned += rows.size();
                        a.found += rows.size();
                        for (const auto& row : rows) {
                            if (timed && std::chrono::steady_clock::now() >= deadline) { stop_after_txn = true; ok = false; break; }
                            ++a.acquires;
                            if (!record_lock_result(a, stopped, locks.lock(tid, LockMode::S, KeyRangeEnt::point(row.key), LockKind::Record))) { ok = false; break; }
                            void* value = nullptr;
                            if (!index.lookup(row.key, value)) { ok = false; break; }
                            a.checksum += row.key ^ *static_cast<uint64_t*>(value);
                        }
                    } else {
                        const LockMode mode = (op.kind == OperationKind::Read) ? LockMode::S : LockMode::X;
                        ++a.acquires;
                        if (!record_lock_result(a, stopped, locks.lock(tid, mode, KeyRangeEnt::point(op.key), LockKind::Record))) { ok = false; break; }
                        if (op.kind == OperationKind::Read) {
                            ++a.reads;
                            void* value = nullptr;
                            if (index.lookup(op.key, value)) { ++a.found; a.checksum += op.key ^ *static_cast<uint64_t*>(value); }
                        } else if (op.kind == OperationKind::Update) { ++a.updates; writes.push_back(&op); }
                        else if (op.kind == OperationKind::Insert) { ++a.inserts; writes.push_back(&op); }
                        else if (op.kind == OperationKind::Delete) { ++a.deletes; writes.push_back(&op); }
                    }
                    if (!ok) break;
                }
                finish_transaction(index, locks, tid, writes, ok, a, stopped);
                if (stop_after_txn) break;
            }
        });
    for (auto& worker : workers) worker.join();
    return merge_parts(parts, begin);
}

RunStats run_gap_lock(BTreeIndex& index, LockManager& locks, const WorkloadTrace& trace,
                      size_t threads) {
    if (!threads) threads = 1;
    SeedValues seed;
    if (!seed_index(index, trace, seed)) { RunStats failed; failed.index_errors = 1; return failed; }
    if (trace.transactions().empty()) return {};
    std::atomic<bool> stopped{false};
    const size_t worker_count = std::min(threads, std::max<size_t>(1, trace.transactions().size()));
    std::vector<LocalAcc> parts(worker_count);
    const auto begin = std::chrono::steady_clock::now();
    const size_t total_txns = trace.transactions().size();
    const bool timed = trace.config().duration_seconds != 0;
    const auto deadline = timed ? std::chrono::steady_clock::now() +
        std::chrono::seconds(trace.config().duration_seconds)
        : std::chrono::steady_clock::time_point::max();
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < worker_count; ++worker)
        workers.emplace_back([&, worker] {
            LocalAcc& a = parts[worker];
            auto acquire = [&](txnid_t txn, LockMode mode, const KeyRangeEnt& range, LockKind kind) {
                ++a.acquires;
                if (!record_lock_result(a, stopped, locks.lock(txn, mode, range, kind))) return false;
                return true;
            };
            for (size_t i = worker, rounds = 0;; i += worker_count, ++rounds) {
                if (stopped.load(std::memory_order_relaxed)) break;
                a.held = 0;
                if (timed) { if ((rounds % 64) == 0 && std::chrono::steady_clock::now() >= deadline) break; }
                else if (i >= total_txns) break;
                const auto& txn = trace.transactions()[i % total_txns];
                const txnid_t tid = txn.txn_id +
                    static_cast<txnid_t>(i / total_txns) * static_cast<txnid_t>(total_txns);
                bool ok = true;
                bool stop_after_txn = false;
                std::vector<const Operation*> writes;
                for (const Operation& op : txn.operations) {
                    if (timed && std::chrono::steady_clock::now() >= deadline) { stop_after_txn = true; ok = false; break; }
                    ++a.ops;
                    if (op.kind == OperationKind::RangeRead) {
                        ++a.ranges;
                        if (!acquire(tid, LockMode::S, KeyRangeEnt::open(op.key, op.end_key), LockKind::Gap) ||
                            !acquire(tid, LockMode::S, KeyRangeEnt::point(op.key), LockKind::Gap) ||
                            !acquire(tid, LockMode::S, KeyRangeEnt::point(op.end_key), LockKind::Gap)) { ok = false; break; }
                        const auto rows = index.scan_range(KeyRangeEnt::half_open(op.key, op.end_key));
                        a.scanned += rows.size();
                        a.found += rows.size();
                        for (const auto& row : rows) {
                            if (timed && std::chrono::steady_clock::now() >= deadline) { stop_after_txn = true; ok = false; break; }
                            if (!acquire(tid, LockMode::S, KeyRangeEnt::point(row.key), LockKind::Record)) { ok = false; break; }
                            void* value = nullptr;
                            if (!index.lookup(row.key, value)) { ok = false; break; }
                            a.checksum += row.key ^ *static_cast<uint64_t*>(value);
                        }
                    } else if (op.kind == OperationKind::Read) {
                        if (!acquire(tid, LockMode::S, KeyRangeEnt::point(op.key), LockKind::Record)) { ok = false; break; }
                        ++a.reads;
                        void* value = nullptr;
                        if (index.lookup(op.key, value)) { ++a.found; a.checksum += op.key ^ *static_cast<uint64_t*>(value); }
                    } else {
                        const LockKind kind = op.kind == OperationKind::Insert ? LockKind::Gap : LockKind::Record;
                        if (!acquire(tid, LockMode::X, KeyRangeEnt::point(op.key), kind)) { ok = false; break; }
                        if (op.kind == OperationKind::Update) { ++a.updates; writes.push_back(&op); }
                        else if (op.kind == OperationKind::Insert) { ++a.inserts; writes.push_back(&op); }
                        else if (op.kind == OperationKind::Delete) { ++a.deletes; writes.push_back(&op); }
                    }
                    if (!ok) break;
                }
                finish_transaction(index, locks, tid, writes, ok, a, stopped);
                if (stop_after_txn) break;
            }
        });
    for (auto& worker : workers) worker.join();
    return merge_parts(parts, begin);
}

RunStats run_next_key_lock(BTreeIndex& index, IRangeLockManager& locks,
                           const WorkloadTrace& trace, size_t threads) {
    if (!threads) threads = 1;
    SeedValues seed;
    if (!seed_index(index, trace, seed)) { RunStats failed; failed.index_errors = 1; return failed; }
    if (trace.transactions().empty()) return {};
    std::atomic<bool> stopped{false};
    const size_t worker_count = std::min(threads, std::max<size_t>(1, trace.transactions().size()));
    std::vector<LocalAcc> parts(worker_count);
    const auto begin = std::chrono::steady_clock::now();
    const size_t total_txns = trace.transactions().size();
    const bool timed = trace.config().duration_seconds != 0;
    const auto deadline = timed ? std::chrono::steady_clock::now() +
        std::chrono::seconds(trace.config().duration_seconds)
        : std::chrono::steady_clock::time_point::max();
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&, worker] {
            LocalAcc& a = parts[worker];
            auto acquire = [&](txnid_t txn, LockMode mode, const KeyRangeEnt& range) {
                ++a.acquires;
                if (!record_lock_result(a, stopped, locks.lock(txn, mode, range, LockKind::NextKey))) return false;
                return true;
            };
            for (size_t i = worker, rounds = 0;; i += worker_count, ++rounds) {
                if (stopped.load(std::memory_order_relaxed)) break;
                a.held = 0;
                if (timed) { if ((rounds % 64) == 0 && std::chrono::steady_clock::now() >= deadline) break; }
                else if (i >= total_txns) break;
                const auto& txn = trace.transactions()[i % total_txns];
                const txnid_t tid = txn.txn_id +
                    static_cast<txnid_t>(i / total_txns) * static_cast<txnid_t>(total_txns);
                bool ok = true;
                bool stop_after_txn = false;
                std::vector<const Operation*> writes;
                for (const Operation& op : txn.operations) {
                    if (timed && std::chrono::steady_clock::now() >= deadline) { stop_after_txn = true; ok = false; break; }
                    ++a.ops;
                    if (op.kind == OperationKind::RangeRead) {
                        ++a.ranges;
                        const auto rows = index.scan_range(KeyRangeEnt::half_open(op.key, op.end_key));
                        a.scanned += rows.size();
                        a.found += rows.size();
                        idx_key_t previous = op.key;
                        // The lower-bound point covers an empty leading gap;
                        // the following (prev,curr] intervals retain next-key semantics.
                        if (!acquire(tid, LockMode::S, KeyRangeEnt::point(op.key))) {
                            ok = false;
                        }
                        for (const auto& row : rows) {
                            if (timed && std::chrono::steady_clock::now() >= deadline) { stop_after_txn = true; ok = false; break; }
                            if (!ok) break;
                            const KeyRangeEnt interval = row.key == previous
                                ? KeyRangeEnt::point(row.key)
                                : right_closed(previous, row.key);
                            if (!acquire(tid, LockMode::S, interval)) { ok = false; break; }
                            previous = row.key;
                        }
                        if (ok) {
                            // The supremum next-key interval protects the tail
                            // gap, including an insertion at the range end.
                            const KeyRangeEnt tail = (previous == op.end_key)
                                ? KeyRangeEnt::point(previous)
                                : right_closed(previous, op.end_key);
                            if (!acquire(tid, LockMode::S, tail)) ok = false;
                            if (ok) {
                                // Recheck after installing locks so a writer
                                // that committed in the scan-to-lock window
                                // cannot create an undetected phantom.
                                const auto verify = index.scan_range(KeyRangeEnt::half_open(op.key, op.end_key));
                                a.rechecked += verify.size();
                                if (verify.size() != rows.size()) ok = false;
                                else for (size_t n = 0; n < rows.size(); ++n)
                                    if (verify[n].key != rows[n].key) { ok = false; break; }
                                // The candidate scan predates the locks. Values
                                // belong to the validated scan under next-key locks.
                                if (ok) for (const auto& row : verify)
                                    a.checksum += row.key ^ *static_cast<uint64_t*>(row.value);
                            }
                        }
                    } else if (op.kind == OperationKind::Read) {
                        if (!acquire(tid, LockMode::S, KeyRangeEnt::point(op.key))) { ok = false; break; }
                        ++a.reads;
                        void* value = nullptr;
                        if (index.lookup(op.key, value)) { ++a.found; a.checksum += op.key ^ *static_cast<uint64_t*>(value); }
                    } else {
                        if (!acquire(tid, LockMode::X, KeyRangeEnt::point(op.key))) { ok = false; break; }
                        if (op.kind == OperationKind::Update) { ++a.updates; writes.push_back(&op); }
                        else if (op.kind == OperationKind::Insert) { ++a.inserts; writes.push_back(&op); }
                        else if (op.kind == OperationKind::Delete) { ++a.deletes; writes.push_back(&op); }
                    }
                    if (!ok) break;
                }
                finish_transaction(index, locks, tid, writes, ok, a, stopped);
                if (stop_after_txn) break;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    return merge_parts(parts, begin);
}

namespace {
struct HierarchyProgress {
    std::uint64_t max_no_commit_ns{0}, last_commit_ns{0}, abort_streak{0}, max_abort_streak{0};
    std::uint64_t max_acquire_ns{0}, max_release_ns{0};
    std::array<std::uint64_t, 3> conflicts{};
    detail::HierarchyRequest last_conflict{LockMode::IS, KeyRangeEnt::unbounded(), LockKind::Node, 0};
    bool has_conflict{false};
};
}

RunStats run_hierarchical_lock(BTreeIndex& index, IRangeLockManager& locks,
                               const WorkloadTrace& trace, size_t threads) {
    if (!threads) threads = 1;
    SeedValues seed;
    if (!seed_index(index, trace, seed)) { RunStats failed; failed.index_errors = 1; return failed; }
    if (trace.transactions().empty()) return {};
    // Planning, sorting, deduplication and storage allocation precede all locks
    // and the timed window. Both local and RDMA use this exact plan.
    std::vector<std::vector<detail::HierarchyRequest>> plans;
    plans.reserve(trace.transactions().size());
    size_t max_ops = 0;
    for (const auto& txn : trace.transactions()) {
        plans.push_back(detail::hierarchy_plan(txn, trace.config().hierarchy_bucket_width));
        max_ops = std::max(max_ops, txn.operations.size());
    }
    std::atomic<bool> stopped{false};
    const size_t worker_count = std::min(threads, trace.transactions().size());
    std::vector<LocalAcc> parts(worker_count);
    const bool diagnose = trace.config().hierarchy_diagnostics;
    std::vector<HierarchyProgress> progress(diagnose ? worker_count : 0);
    const auto begin = std::chrono::steady_clock::now();
    const size_t total_txns = trace.transactions().size();
    const bool timed = trace.config().duration_seconds != 0;
    const auto deadline = timed ? begin + std::chrono::seconds(trace.config().duration_seconds)
                               : std::chrono::steady_clock::time_point::max();
    auto elapsed = [&] {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin).count());
    };
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&, worker] {
            LocalAcc& a = parts[worker];
            std::vector<const Operation*> writes;
            writes.reserve(max_ops);
            for (size_t i = worker, rounds = 0;; i += worker_count, ++rounds) {
                if (stopped.load(std::memory_order_relaxed)) break;
                a.held = 0;
                if (timed) { if ((rounds % 64) == 0 && std::chrono::steady_clock::now() >= deadline) break; }
                else if (i >= total_txns) break;
                const auto& txn = trace.transactions()[i % total_txns];
                const txnid_t tid = txn.txn_id +
                    static_cast<txnid_t>(i / total_txns) * static_cast<txnid_t>(total_txns);
                bool ok = true, stop_after_txn = false;
                writes.clear();
                for (const auto& request : plans[i % total_txns]) {
                    if (timed && std::chrono::steady_clock::now() >= deadline) { stop_after_txn = true; ok = false; break; }
                    ++a.acquires;
                    const auto started = diagnose ? elapsed() : 0;
                    const auto rc = locks.lock(tid, request.mode, request.range, request.kind);
                    if (diagnose) {
                        auto& d = progress[worker];
                        d.max_acquire_ns = std::max(d.max_acquire_ns, elapsed() - started);
                        if (rc == AbortConflict) {
                            ++d.conflicts[request.phase]; d.last_conflict = request; d.has_conflict = true;
                        }
                    }
                    if (!record_lock_result(a, stopped, rc)) { ok = false; break; }
                }
                if (ok) for (const Operation& op : txn.operations) {
                    if (timed && std::chrono::steady_clock::now() >= deadline) { stop_after_txn = true; ok = false; break; }
                    ++a.ops;
                    if (op.kind == OperationKind::RangeRead) {
                        ++a.ranges;
                        const auto rows = index.scan_range(KeyRangeEnt::half_open(op.key, op.end_key));
                        a.scanned += rows.size(); a.found += rows.size();
                        for (const auto& row : rows) {
                            if (timed && std::chrono::steady_clock::now() >= deadline) { stop_after_txn = true; ok = false; break; }
                            a.checksum += row.key ^ *static_cast<uint64_t*>(row.value);
                        }
                    } else if (op.kind == OperationKind::Read) {
                        ++a.reads;
                        void* value = nullptr;
                        if (index.lookup(op.key, value)) { ++a.found; a.checksum += op.key ^ *static_cast<uint64_t*>(value); }
                    } else {
                        if (op.kind == OperationKind::Update) ++a.updates;
                        else if (op.kind == OperationKind::Insert) ++a.inserts;
                        else ++a.deletes;
                        writes.push_back(&op);
                    }
                    if (!ok) break;
                }
                const auto committed_before = a.committed;
                const auto aborted_before = a.aborted;
                std::uint64_t release_ns = 0;
                finish_transaction(index, locks, tid, writes, ok, a, stopped, diagnose ? &release_ns : nullptr);
                if (diagnose) {
                    auto& d = progress[worker];
                    const auto now = elapsed();
                    d.max_no_commit_ns = std::max(d.max_no_commit_ns, now - d.last_commit_ns);
                    d.max_release_ns = std::max(d.max_release_ns, release_ns);
                    if (a.committed != committed_before) { d.last_commit_ns = now; d.abort_streak = 0; }
                    else if (a.aborted != aborted_before) {
                        ++d.abort_streak; d.max_abort_streak = std::max(d.max_abort_streak, d.abort_streak);
                    }
                }
                if (stop_after_txn) break;
            }
            if (diagnose) {
                auto& d = progress[worker];
                d.max_no_commit_ns = std::max(d.max_no_commit_ns, elapsed() - d.last_commit_ns);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    const auto result = merge_parts(parts, begin);
    // Emit only after all transactions ended. JSONL stays off the CSV channel.
    if (diagnose) for (size_t worker = 0; worker < worker_count; ++worker) {
        const auto& a = parts[worker]; const auto& d = progress[worker];
        std::cerr << "{\"kind\":\"hierarchy-progress\",\"worker\":" << worker
            << ",\"attempts\":" << a.tx << ",\"committed\":" << a.committed
            << ",\"aborted\":" << a.aborted << ",\"failed_transactions\":" << a.failed_tx
            << ",\"max_no_commit_ns\":" << d.max_no_commit_ns
            << ",\"max_abort_streak\":" << d.max_abort_streak << ",\"upgrade_requests\":0"
            << ",\"root_conflicts\":" << d.conflicts[0] << ",\"bucket_conflicts\":" << d.conflicts[1]
            << ",\"record_conflicts\":" << d.conflicts[2] << ",\"max_acquire_ns\":" << d.max_acquire_ns
            << ",\"max_release_ns\":" << d.max_release_ns << ",\"last_conflict\":";
        if (!d.has_conflict) std::cerr << "null";
        else std::cerr << "{\"phase\":" << d.last_conflict.phase
            << ",\"mode\":" << static_cast<unsigned>(d.last_conflict.mode)
            << ",\"range\":\"" << d.last_conflict.range.to_string() << "\"}";
        std::cerr << "}\n";
    }
    return result;
}

}  // namespace rrl
