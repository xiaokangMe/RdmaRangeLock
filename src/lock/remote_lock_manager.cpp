#include "lock/remote_lock_manager.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <limits>
#include <thread>
#include <unordered_map>

namespace rrl {
namespace {

using ProfileClock = std::chrono::steady_clock;
ProfileClock::time_point profile_start(bool enabled) {
    return enabled ? ProfileClock::now() : ProfileClock::time_point{};
}
void profile_add(bool enabled, ProfileClock::time_point start, std::atomic<std::uint64_t>& total) {
    if (enabled) total.fetch_add(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - start).count()),
        std::memory_order_relaxed);
}
// A successful connection generation is unique across object lifetimes, even
// when an allocator reconstructs a manager at the same address.
std::atomic<std::uint64_t> next_connection_generation{1};
constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();

bool same_range(const KeyRangeEnt& lhs, const KeyRangeEnt& rhs) {
    return lhs.StartKey == rhs.StartKey && lhs.EndKey == rhs.EndKey &&
           lhs.has_start == rhs.has_start && lhs.has_end == rhs.has_end &&
           lhs.start_bound == rhs.start_bound && lhs.end_bound == rhs.end_bound;
}

bool same_lock(const RangeLock& lhs, const RangeLock& rhs) {
    return lhs.txn_id == rhs.txn_id && lhs.kind == rhs.kind &&
           same_range(lhs.range, rhs.range);
}

std::uint64_t make_token(const void* address, std::size_t slot) {
    const auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto salt = static_cast<std::uint64_t>(slot + 1) * 0x9e3779b97f4a7c15ULL;
    const auto token = (now ^ reinterpret_cast<std::uintptr_t>(address) ^ salt) &
                       0x7fffffffffffffffULL;
    return token == 0 ? static_cast<std::uint64_t>(slot + 1) : token;
}

}  // namespace

struct RemoteLockManager::LoadedShard {
    std::size_t shard{0};
    std::vector<RemoteLockSlot> slots;
    SlotIndex index;
};

struct RemoteLockManager::Workspace {
    std::vector<LoadedShard> tables;
    std::array<std::vector<RemoteLockSlot>, kRemoteLockShards> moved;
    std::vector<std::size_t> acquired;
    std::vector<std::uint64_t> previous;
    std::vector<RdmaCompareSwapRequest> swaps;
    std::vector<RdmaReadRequest> reads;
    std::vector<RdmaWriteRequest> writes, chunk;
    std::vector<txnid_t> conflicts;
    std::array<std::uint64_t, kRemoteLockShards> counts{};
    std::array<RemoteLockSlot, kRemoteLockShards> payload{};
    std::array<std::size_t, kRemoteLockShards> owned{};

    Workspace() : tables(kRemoteLockShards) {
        for (auto& table : tables) {
            table.slots.reserve(kRemoteLockSlotsPerShard);
            table.index.reserve(kRemoteLockSlotsPerShard);
        }
        for (auto& slots : moved) slots.reserve(kRemoteLockSlotsPerShard);
        acquired.reserve(kRemoteLockShards); previous.reserve(kRemoteLockShards);
        swaps.reserve(kRemoteLockShards); reads.reserve(kRemoteLockShards);
        // Each removed slot moves at most one tail slot; each shard publishes
        // at most one count. Sources remain stable across write chunks.
        writes.reserve(kRemoteLockShards * (kRemoteLockSlotsPerShard + 1));
        chunk.reserve(kRemoteBatchLimit);
        conflicts.reserve(kRemoteLockShards * kRemoteLockSlotsPerShard);
    }
};

RemoteLockManager::ConnectionSlot::ConnectionSlot() : workspace(std::make_unique<Workspace>()) {}
RemoteLockManager::ConnectionSlot::~ConnectionSlot() = default;

class RemoteLockManager::RemoteGateGuard {
public:
    explicit RemoteGateGuard(const RemoteLockManager& manager)
        : manager_(manager), slot_(manager.current_slot()), acquired_(slot_.workspace->acquired) {
        acquired_.clear();
    }
    ~RemoteGateGuard() noexcept {
        release();
    }
    GateOutcome acquire(const std::vector<std::size_t>& ids, bool reliable = false) {
        const auto result = manager_.enter_shards(ids, acquired_, reliable);
        if (result == GateOutcome::Acquired) {
            start_ = profile_start(manager_.profile_enabled_);
            if (manager_.gate_state_test_hook_) manager_.gate_state_test_hook_(true);
        }
        return result;
    }
    bool release() noexcept {
        if (acquired_.empty()) return true;
        const bool ok = manager_.leave_shards(acquired_);
        profile_add(manager_.profile_enabled_, start_, manager_.gate_hold_ns_);
        if (manager_.gate_state_test_hook_) manager_.gate_state_test_hook_(false);
        acquired_.clear();
        if (!ok) manager_.invalidate(&slot_);
        return ok;
    }
private:
    const RemoteLockManager& manager_;
    ProfileClock::time_point start_{};
    ConnectionSlot& slot_;
    std::vector<std::size_t>& acquired_;
};

// Serialises this process's threads per shard when enabled. Locks are taken in
// ascending shard order, the same order used by enter_shards, so two threads
// can never wait on each other in opposite directions.
class RemoteLockManager::LocalGateGuard {
public:
    LocalGateGuard(std::array<std::mutex, kRemoteLockShards>& gates,
                   const std::vector<std::size_t>& ids, bool enabled) {
        if (!enabled) return;
        std::size_t i = 0;
        for (const std::size_t shard : ids) locks_[i++] = std::unique_lock<std::mutex>(gates[shard]);
    }
    void release() {
        for (auto& lock : locks_) if (lock.owns_lock()) lock.unlock();
    }

private:
    std::array<std::unique_lock<std::mutex>, kRemoteLockShards> locks_;
};

RemoteLockManager::RemoteLockManager() {
    if (const char* value = std::getenv("RRL_RDMA_PROFILE")) {
        if (std::strcmp(value, "0") && std::strcmp(value, "1"))
            throw std::invalid_argument("RRL_RDMA_PROFILE must be 0 or 1");
        profile_enabled_ = std::strcmp(value, "1") == 0;
    }
    if (const char* value = std::getenv("RRL_SLOT_LOOKUP")) {
        if (std::strcmp(value, "linear") && std::strcmp(value, "index"))
            throw std::invalid_argument("RRL_SLOT_LOOKUP must be linear or index");
        slot_lookup_linear_ = std::strcmp(value, "linear") == 0;
    }
    if (const char* value = std::getenv("RRL_GATE_RETRY")) {
        char* end = nullptr;
        const auto parsed = std::strtoul(value, &end, 10);
        if (end && *end == '\0') gate_retries_ = static_cast<std::size_t>(parsed);
    }
    if (const char* value = std::getenv("RRL_LOCAL_GATE")) {
        local_gate_enabled_ = std::strtoul(value, nullptr, 10) != 0;
    }
}

RemoteLockManager::~RemoteLockManager() = default;

bool RemoteLockManager::connect(const std::string& host, std::uint16_t port) {
    if (connections_.size() != 0) return connected();
    if (const char* env = std::getenv("RRL_RDMA_BUCKET_WIDTH")) {
        char* end = nullptr;
        const auto parsed = std::strtoull(env, &end, 10);
        if (end && *end == '\0' && parsed != 0) bucket_width_ = parsed;
    }
    namespace_token_ = make_token(this, connection_count_) & 0x0000ffffffffffffULL;
    if (namespace_token_ == 0) namespace_token_ = 1;
    txn_cache_.clear();
    connections_.reserve(connection_count_);
    for (std::size_t i = 0; i < connection_count_; ++i) {
        auto slot = std::make_unique<ConnectionSlot>();
        slot->owner_token = make_token(this, i);
        slot->connection = std::make_unique<RdmaConnection>();
        if (!slot->connection->connect_client(host, port, slot->owner_token)) {
            connections_.clear();
            return false;
        }
        if (i == 0) {
            const auto& remote = slot->connection->remote_memory();
            if (remote.length < sizeof(RemoteLockRegion)) {
                connections_.clear();
                return false;
            }
            shards_address_ = remote.address + offsetof(RemoteLockRegion, shards);
        }
        connections_.push_back(std::move(slot));
    }
    next_connection_.store(0, std::memory_order_relaxed);
    connection_generation_ = next_connection_generation.fetch_add(1, std::memory_order_relaxed);
    if (connection_generation_ == 0) std::abort();
    return connected();
}

void RemoteLockManager::set_bucket_width(std::uint64_t width) {
    if (width != 0) bucket_width_ = width;
}

void RemoteLockManager::set_connection_count(std::size_t count) {
    if (connections_.empty()) connection_count_ = std::max<std::size_t>(1, count);
}

bool RemoteLockManager::connected() const {
    if (invalid_.load(std::memory_order_acquire) || connections_.empty()) return false;
    for (const auto& slot : connections_)
        if (!slot || !slot->connection || !slot->connection->connected()) return false;
    return true;
}

std::uint64_t RemoteLockManager::rdma_timeouts() const {
    std::uint64_t total = 0;
    for (const auto& slot : connections_)
        if (slot && slot->connection) total += slot->connection->timeout_count();
    return total;
}

std::uint64_t RemoteLockManager::rdma_completion_errors() const {
    std::uint64_t total = 0;
    for (const auto& slot : connections_)
        if (slot && slot->connection) total += slot->connection->completion_error_count();
    return total;
}

std::uint64_t RemoteLockManager::rdma_operations() const {
    std::uint64_t total = 0;
    for (const auto& slot : connections_)
        if (slot && slot->connection) total += slot->connection->operation_count();
    return total;
}

RemoteLockManager::Diagnostics RemoteLockManager::diagnostics() const {
    Diagnostics result;
    result.gate_attempts = gate_attempts_.load(std::memory_order_relaxed);
    result.gate_retries = gate_retry_count_.load(std::memory_order_relaxed);
    result.gate_aborts = gate_abort_count_.load(std::memory_order_relaxed);
    result.batches = batch_count_.load(std::memory_order_relaxed);
    result.gate_transport_errors = gate_transport_errors_.load(std::memory_order_relaxed);
    result.count_errors = count_errors_.load(std::memory_order_acquire);
    result.last_count_error = last_count_error_.load(std::memory_order_relaxed);
    result.local_errors = local_errors_.load(std::memory_order_relaxed);
    result.profile_enabled = profile_enabled_;
    result.slot_lookup_linear = slot_lookup_linear_;
    result.lock_calls = lock_calls_.load(std::memory_order_relaxed);
    result.cache_hits = cache_hits_.load(std::memory_order_relaxed);
    result.io_wait_ns = io_wait_ns_.load(std::memory_order_relaxed);
    result.local_gate_wait_ns = local_gate_wait_ns_.load(std::memory_order_relaxed);
    result.gate_hold_ns = gate_hold_ns_.load(std::memory_order_relaxed);
    result.index_build_ns = index_build_ns_.load(std::memory_order_relaxed);
    result.lookup_ns = lookup_ns_.load(std::memory_order_relaxed);
    result.slot_checks = slot_checks_.load(std::memory_order_relaxed);
    result.txn_wait_ns = txn_wait_ns_.load(std::memory_order_relaxed);
    for (const auto& slot : connections_) {
        const auto p = slot->connection->profile();
        result.sync_calls += p.sync_calls; result.sync_ns += p.sync_ns;
        result.read_bytes += p.read_bytes; result.write_bytes += p.write_bytes;
        for (std::size_t i = 0; i < result.phase_calls.size(); ++i) {
            result.phase_calls[i] += p.phase_calls[i]; result.phase_ns[i] += p.phase_ns[i];
        }
    }
    return result;
}

RdmaFailureInfo RemoteLockManager::last_failure() const {
    RdmaFailureInfo latest{};
    for (const auto& slot : connections_) {
        if (!slot || !slot->connection) continue;
        const auto snapshot = slot->connection->last_failure();
        if (snapshot.sequence > latest.sequence) latest = snapshot;
    }
    return latest;
}

std::int64_t RemoteLockManager::last_failure_shard() const {
    const RdmaFailureInfo failure = last_failure();
    if (failure.remote_address < shards_address_) return -1;
    const std::uint64_t offset = failure.remote_address - shards_address_;
    if (offset >= kRemoteLockShards * sizeof(RemoteLockShard)) return -1;
    return static_cast<std::int64_t>(offset / sizeof(RemoteLockShard));
}

std::uint64_t RemoteLockManager::timeouts_by_tag(RdmaOpTag tag) const {
    std::uint64_t total = 0;
    for (const auto& slot : connections_)
        if (slot && slot->connection) total += slot->connection->timeout_count(tag);
    return total;
}

RemoteLockManager::ConnectionSlot& RemoteLockManager::current_slot() const {
    struct Assignment {
        std::size_t index{0};
        std::uint64_t generation{0};
    };
    static thread_local std::unordered_map<const RemoteLockManager*, Assignment> assignments;
    auto it = assignments.find(this);
    if (it == assignments.end() || it->second.generation != connection_generation_ ||
        it->second.index >= connections_.size()) {
        const auto index = next_connection_.fetch_add(1, std::memory_order_relaxed) % connections_.size();
        if (it == assignments.end()) it = assignments.emplace(this, Assignment{index, connection_generation_}).first;
        else it->second = Assignment{index, connection_generation_};
    }
    return *connections_[it->second.index];
}

RdmaConnection& RemoteLockManager::current_connection() const {
    return *current_slot().connection;
}

std::uint64_t RemoteLockManager::current_owner_token() const {
    return current_slot().owner_token;
}

std::uint64_t RemoteLockManager::current_namespace_token() const {
    return namespace_token_;
}

bool RemoteLockManager::owns_owner_token(std::uint64_t owner) const {
    for (const auto& slot : connections_)
        if (slot && slot->owner_token == owner) return true;
    return false;
}

std::vector<std::size_t> RemoteLockManager::shard_ids(const KeyRangeEnt& range) const {
    std::vector<std::size_t> ids;
    if (!range.has_start || !range.has_end) {
        ids.resize(kRemoteLockShards);
        for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = i;
        return ids;
    }
    const std::uint64_t first = range.StartKey / bucket_width_;
    std::uint64_t last = range.EndKey / bucket_width_;
    if (range.end_bound == Bound::Open && range.EndKey != 0 &&
        range.EndKey % bucket_width_ == 0) {
        --last;
    }
    if (last < first || last - first + 1 >= kRemoteLockShards) {
        ids.resize(kRemoteLockShards);
        for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = i;
        return ids;
    }
    for (std::uint64_t bucket = first;; ++bucket) {
        const std::size_t shard = static_cast<std::size_t>(bucket % kRemoteLockShards);
        if (std::find(ids.begin(), ids.end(), shard) == ids.end()) ids.push_back(shard);
        if (bucket == last) break;
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<std::size_t> RemoteLockManager::shard_ids(const KeyRangeEnt& range,
                                                     LockKind kind,
                                                     LockMode mode) const {
    // Root intention locks are metadata locks. They need one stable shard for
    // compatibility checks; copying them to every data shard only creates a
    // needless all-shards gate operation.
    if (kind == LockKind::Node && range.is_unbounded() &&
        (mode == LockMode::IS || mode == LockMode::IX)) return {0};
    return shard_ids(range);
}

std::vector<std::size_t> RemoteLockManager::shards_for_txn(txnid_t txn_id) const {
    return txn_cache_.shards_or_all(txn_id, kRemoteLockShards);
}

void RemoteLockManager::refresh_txn_shards(txnid_t txn_id) const {
    txn_cache_.refresh_shards(txn_id, [this](const TxnLockCache::CachedLock& held) {
        return shard_ids(held.range, held.kind, held.mode);
    });
}

std::uint64_t RemoteLockManager::shard_base(std::size_t shard) const {
    return shards_address_ + shard * sizeof(RemoteLockShard);
}

std::uint32_t RemoteLockManager::current_shards_rkey() const {
    return current_connection().remote_memory().rkey;
}

RemoteLockManager::GateOutcome RemoteLockManager::try_enter_shards(
    const std::vector<std::size_t>& ids, std::vector<std::size_t>& acquired) const {
    acquired.clear();
    if (ids.empty()) return GateOutcome::Acquired;
    auto& connection = current_connection();
    connection.set_operation_tag(RdmaOpTag::Gate);
    const auto owner = current_owner_token();
    if (ids.size() == 1) {
        std::uint64_t previous = 0;
        if (!connection.compare_swap(shard_base(ids.front()) + offsetof(RemoteLockShard, gate),
                                     current_shards_rkey(), 0, owner, previous)) {
            // Failure quiesces/notifies the peer; server recovery reclaims
            // ownership. Never post cleanup on the failed QP.
            return GateOutcome::TransportError;
        }
        if (previous != 0) return GateOutcome::Busy;
        acquired.push_back(ids.front());
        return GateOutcome::Acquired;
    }
    auto& workspace = *current_slot().workspace;
    auto& previous = workspace.previous;
    auto& requests = workspace.swaps;
    previous.assign(ids.size(), 0);
    requests.clear();
    for (std::size_t i = 0; i < ids.size(); ++i) {
        requests.push_back({shard_base(ids[i]) + offsetof(RemoteLockShard, gate),
                            current_shards_rkey(), 0, owner, &previous[i]});
    }
    if (!connection.compare_swap_batch(requests)) {
        // A posted prefix may have acquired gates: peer recovery owns cleanup.
        return GateOutcome::TransportError;
    }
    for (std::size_t i = 0; i < ids.size(); ++i)
        if (previous[i] == 0) acquired.push_back(ids[i]);
    if (acquired.size() != ids.size()) {
        const bool released = leave_shards(acquired);
        acquired.clear();
        return released ? GateOutcome::Busy : GateOutcome::TransportError;
    }
    return GateOutcome::Acquired;
}

RemoteLockManager::GateOutcome RemoteLockManager::enter_shards(
    const std::vector<std::size_t>& ids, std::vector<std::size_t>& acquired,
    bool reliable) const {
    if (ids.empty()) { acquired.clear(); return GateOutcome::Acquired; }
    // A no-wait gate is the correctness boundary, but aborting on the first
    // collision turns a short queue into a false conflict. lock() therefore
    // keeps a bounded spin: unbounded blocking there would turn the
    // all-or-nothing multi-shard acquisition into a deadlock risk. The release
    // path is different - giving up leaves the locks behind it published for
    // good - so it waits with backoff until the gate frees up.
    gate_attempts_.fetch_add(1, std::memory_order_relaxed);
    const std::size_t limit = reliable ? 4096 : gate_retries_;
    for (std::size_t attempt = 0;; ++attempt) {
        const GateOutcome outcome = try_enter_shards(ids, acquired);
        if (outcome == GateOutcome::Acquired) return outcome;
        if (outcome == GateOutcome::TransportError) {
            gate_transport_errors_.fetch_add(1, std::memory_order_relaxed);
            return outcome;
        }
        if (attempt >= limit) break;
        gate_retry_count_.fetch_add(1, std::memory_order_relaxed);
        if (reliable) {
            const auto backoff = std::chrono::microseconds(
                std::size_t{1} << std::min<std::size_t>(attempt, 7));
            std::this_thread::sleep_for(backoff);
        } else if (attempt < 4) {
            const unsigned spins = 1u << std::min<std::size_t>(attempt, 4);
            for (unsigned spin = 0; spin < spins; ++spin) std::this_thread::yield();
        } else {
            // A gate is held for a few RDMA round trips, far longer than a
            // yield. Sleeping a short, capped interval is what actually lets a
            // contending request through instead of aborting the transaction.
            const auto backoff = std::chrono::microseconds(
                std::min<std::size_t>(std::size_t{1} << std::min<std::size_t>(attempt - 3, 6), 64));
            std::this_thread::sleep_for(backoff);
        }
    }
    gate_abort_count_.fetch_add(1, std::memory_order_relaxed);
    return GateOutcome::Busy;
}

bool RemoteLockManager::leave_shards(const std::vector<std::size_t>& acquired) const {
    if (acquired.empty()) return true;
    auto& connection = current_connection();
    connection.set_operation_tag(RdmaOpTag::ReleaseGate);
    const auto owner = current_owner_token();
    if (acquired.size() == 1) {
        std::uint64_t previous = 0;
        return connection.compare_swap(shard_base(acquired.front()) + offsetof(RemoteLockShard, gate),
                                       current_shards_rkey(), owner, 0, previous) && previous == owner;
    }
    auto& workspace = *current_slot().workspace;
    auto& previous = workspace.previous;
    auto& requests = workspace.swaps;
    previous.assign(acquired.size(), 0);
    requests.clear();
    for (std::size_t i = 0; i < acquired.size(); ++i) {
        requests.push_back({shard_base(acquired[acquired.size() - 1 - i]) + offsetof(RemoteLockShard, gate),
                            current_shards_rkey(), owner, 0, &previous[i]});
    }
    if (!connection.compare_swap_batch(requests)) return false;
    return std::all_of(previous.begin(), previous.end(), [owner](std::uint64_t value) {
        return value == owner;
    });
}

bool RemoteLockManager::load_tables(const std::vector<std::size_t>& ids,
                                    std::vector<LoadedShard>& tables) const {
    // Keep all shard buffers alive when the next request touches fewer shards.
    for (auto& table : tables) table.slots.clear();
    auto& connection = current_connection();
    const std::uint32_t rkey = current_shards_rkey();

    auto& workspace = *current_slot().workspace;
    auto& counts = workspace.counts;
    counts.fill(0);
    if (ids.size() == 1) {
        connection.set_operation_tag(RdmaOpTag::LoadCounts);
        if (!connection.read(shard_base(ids.front()) + offsetof(RemoteLockShard, active_slots),
                             rkey, &counts.front(), sizeof(std::uint64_t)))
            return false;
    } else {
        auto& reads = workspace.reads;
        reads.clear();
        for (std::size_t i = 0; i < ids.size(); ++i)
            reads.push_back({shard_base(ids[i]) + offsetof(RemoteLockShard, active_slots), rkey,
                             &counts[i], sizeof(std::uint64_t)});
        connection.set_operation_tag(RdmaOpTag::LoadCounts);
        if (!connection.read_batch(reads)) return false;
        batch_count_.fetch_add(1, std::memory_order_relaxed);
    }

    auto& slot_reads = workspace.reads;
    slot_reads.clear();
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (counts[i] > kRemoteLockSlotsPerShard) return false;
        tables[i].shard = ids[i];
        tables[i].slots.resize(static_cast<std::size_t>(counts[i]));
        if (tables[i].slots.empty()) continue;
        const std::size_t length = tables[i].slots.size() * sizeof(RemoteLockSlot);
        // Keep the batch inside the transport scratch buffer; oversized tables
        // are read one shard at a time.
        if (bytes != 0 && bytes + length > (1u << 20)) {
            connection.set_operation_tag(RdmaOpTag::LoadSlots);
            if (!connection.read_batch(slot_reads)) return false;
            batch_count_.fetch_add(1, std::memory_order_relaxed);
            slot_reads.clear();
            bytes = 0;
        }
        slot_reads.push_back({shard_base(ids[i]) + offsetof(RemoteLockShard, slots), rkey,
                              tables[i].slots.data(), length});
        bytes += length;
    }
    if (!slot_reads.empty()) {
        connection.set_operation_tag(RdmaOpTag::LoadSlots);
        if (!connection.read_batch(slot_reads)) return false;
        if (slot_reads.size() > 1) batch_count_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool RemoteLockManager::write_chunks(const std::vector<RdmaWriteRequest>& writes) const {
    if (writes.empty()) return true;
    auto& connection = current_connection();
    connection.set_operation_tag(RdmaOpTag::StoreSlot);
    for (std::size_t offset = 0; offset < writes.size(); offset += kRemoteBatchLimit) {
        const std::size_t count = std::min(kRemoteBatchLimit, writes.size() - offset);
        auto& chunk = current_slot().workspace->chunk;
        chunk.assign(writes.begin() + static_cast<std::ptrdiff_t>(offset),
                     writes.begin() + static_cast<std::ptrdiff_t>(offset + count));
        if (!connection.write_batch(chunk)) return false;
        if (count > 1) batch_count_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

RangeLock RemoteLockManager::decode(const RemoteLockSlot& slot) {
    return decode_remote_slot(slot);
}

void RemoteLockManager::encode(RemoteLockSlot& slot, std::uint64_t owner,
                               txnid_t txn_id, LockMode mode, LockKind kind,
                               const KeyRangeEnt& range) {
    encode_remote_slot(slot, owner, txn_id, mode, kind, range);
}

txnid_t RemoteLockManager::wire_txn(txnid_t txn_id) const {
    return (current_namespace_token() << 16) ^ (txn_id & 0x0000ffffffffffffULL);
}

void RemoteLockManager::invalidate(ConnectionSlot* slot) const noexcept {
    invalid_.store(true, std::memory_order_release);
    local_errors_.fetch_add(1, std::memory_order_relaxed);
    if (slot && slot->connection) slot->connection->disconnect();
}

RC RemoteLockManager::lock(txnid_t txn_id, LockMode mode, const KeyRangeEnt& range,
                           LockKind kind, std::vector<txnid_t>* conflicts) {
    try {
        if (!connected()) return ERROR;
        const auto txn_queued = profile_start(profile_enabled_);
        std::unique_lock<std::mutex> operation(txn_operation_mutexes_[txn_id % kRemoteTxnCacheShards]);
        profile_add(profile_enabled_, txn_queued, txn_wait_ns_);
        auto& slot = current_slot();
        const auto queued = profile_start(profile_enabled_);
        std::unique_lock<std::mutex> io(slot.io_mutex);
        profile_add(profile_enabled_, queued, io_wait_ns_);
        try { return lock_impl(txn_id, mode, range, kind, conflicts); }
        catch (...) { invalidate(&slot); return ERROR; }
    } catch (...) { invalidate(nullptr); return ERROR; }
}

RC RemoteLockManager::release(txnid_t txn_id, const KeyRangeEnt* range, LockKind kind) {
    try {
        if (!connected()) return ERROR;
        const auto txn_queued = profile_start(profile_enabled_);
        std::unique_lock<std::mutex> operation(txn_operation_mutexes_[txn_id % kRemoteTxnCacheShards]);
        profile_add(profile_enabled_, txn_queued, txn_wait_ns_);
        auto& slot = current_slot();
        const auto queued = profile_start(profile_enabled_);
        std::unique_lock<std::mutex> io(slot.io_mutex);
        profile_add(profile_enabled_, queued, io_wait_ns_);
        try { return release_impl(txn_id, range, kind); }
        catch (...) { invalidate(&slot); return ERROR; }
    } catch (...) { invalidate(nullptr); return ERROR; }
}

std::size_t RemoteLockManager::get_lock_count(txnid_t txn_id) const {
    try {
        if (!connected()) { record_count_error(ERROR); return 0; }
        const auto txn_queued = profile_start(profile_enabled_);
        std::unique_lock<std::mutex> operation(txn_operation_mutexes_[txn_id % kRemoteTxnCacheShards]);
        profile_add(profile_enabled_, txn_queued, txn_wait_ns_);
        auto& slot = current_slot();
        const auto queued = profile_start(profile_enabled_);
        std::unique_lock<std::mutex> io(slot.io_mutex);
        profile_add(profile_enabled_, queued, io_wait_ns_);
        try { return count_impl(txn_id); }
        catch (...) { invalidate(&slot); record_count_error(ERROR); return 0; }
    } catch (...) { invalidate(nullptr); record_count_error(ERROR); return 0; }
}

RC RemoteLockManager::lock_impl(txnid_t txn_id, LockMode mode, const KeyRangeEnt& range,
                           LockKind kind, std::vector<txnid_t>* conflicts) {
    if (!connected() || range.empty() || !range.valid()) return ERROR;
    if (conflicts) conflicts->clear();
    if (profile_enabled_) lock_calls_.fetch_add(1, std::memory_order_relaxed);
    if (txn_cache_.satisfies(txn_id, mode, range, kind)) {
        if (profile_enabled_) cache_hits_.fetch_add(1, std::memory_order_relaxed);
        return RCOK;
    }
    const auto ids = shard_ids(range, kind, mode);
    const auto queued_gate = profile_start(profile_enabled_);
    LocalGateGuard local(gate_mutexes_, ids, local_gate_enabled_);
    profile_add(profile_enabled_, queued_gate, local_gate_wait_ns_);
    RemoteGateGuard gate(*this);
    switch (gate.acquire(ids)) {
        case GateOutcome::Acquired: break;
        case GateOutcome::Busy: return AbortGate;
        case GateOutcome::TransportError: return ERROR;
    }

    if (after_gates_test_hook_) after_gates_test_hook_();
    auto& workspace = *current_slot().workspace;
    auto& tables = workspace.tables;
    if (!load_tables(ids, tables)) {
        gate.release();
        return ERROR;
    }

    auto& connection = current_connection();
    const auto owner = current_owner_token();
    const auto rkey = current_shards_rkey();
    const txnid_t id = wire_txn(txn_id);
    const RangeLock request{id, mode, kind, range};

    // Time the complete rebuild, not just the subsequent binary searches.
    const auto build_start = profile_start(profile_enabled_);
    for (std::size_t n = 0; n < ids.size(); ++n)
        tables[n].index.build(tables[n].slots, slot_lookup_linear_);
    profile_add(profile_enabled_, build_start, index_build_ns_);
    const auto lookup_start = profile_start(profile_enabled_);
    std::uint64_t checks = 0;
    bool conflict = false;
    workspace.conflicts.clear();
    for (std::size_t n = 0; n < ids.size(); ++n) {
        const auto& table = tables[n];
        table.index.for_each_candidate(table.slots, range, [&](const RemoteLockSlot& slot) {
            ++checks;
            const RangeLock held = decode(slot);
            if (!request.conflicts(held)) return;
            conflict = true;
            if (conflicts && std::find(workspace.conflicts.begin(), workspace.conflicts.end(), held.txn_id) == workspace.conflicts.end())
                workspace.conflicts.push_back(held.txn_id);
        });
    }
    if (profile_enabled_) slot_checks_.fetch_add(checks, std::memory_order_relaxed);
    if (conflict) {
        profile_add(profile_enabled_, lookup_start, lookup_ns_);
        if (!gate.release()) return ERROR;
        local.release();
        if (conflicts) conflicts->assign(workspace.conflicts.begin(), workspace.conflicts.end());
        return AbortConflict;
    }

    // Capacity preflight: check every target shard before publishing any copy,
    // otherwise a later full shard would leave earlier copies behind.
    auto& owned_slot = workspace.owned;
    owned_slot.fill(kNoSlot);
    for (std::size_t n = 0; n < ids.size(); ++n) {
        std::size_t found = kNoSlot;
        if (tables[n].index.find_owned(tables[n].slots, id, kind, range, found)) {
            owned_slot[n] = found;
            continue;
        }
        if (tables[n].slots.size() >= kRemoteLockSlotsPerShard) {
            profile_add(profile_enabled_, lookup_start, lookup_ns_);
            return gate.release() ? AbortCapacity : ERROR;
        }
    }

    profile_add(profile_enabled_, lookup_start, lookup_ns_);
    auto& payload = workspace.payload;
    auto& counts = workspace.counts;
    auto& writes = workspace.writes;
    writes.clear();
    for (std::size_t n = 0; n < ids.size(); ++n) {
        auto& table = tables[n];
        if (owned_slot[n] != kNoSlot) {
            auto& slot = table.slots[owned_slot[n]];
            slot.mode = static_cast<std::uint8_t>(
                lock_mode_join(static_cast<LockMode>(slot.mode), mode));
            payload[n] = slot;
            writes.push_back({shard_base(table.shard) + offsetof(RemoteLockShard, slots) +
                                  owned_slot[n] * sizeof(RemoteLockSlot),
                              rkey, &payload[n], sizeof(RemoteLockSlot)});
            continue;
        }
        encode_remote_slot(payload[n], owner, id, mode, kind, range);
        const std::size_t index = table.slots.size();
        writes.push_back({shard_base(table.shard) + offsetof(RemoteLockShard, slots) +
                              index * sizeof(RemoteLockSlot),
                          rkey, &payload[n], sizeof(RemoteLockSlot)});
        counts[n] = table.slots.size() + 1;
        writes.push_back({shard_base(table.shard) + offsetof(RemoteLockShard, active_slots),
                          rkey, &counts[n], sizeof(std::uint64_t)});
    }
    (void)connection;
    if (!write_chunks(writes)) {
        gate.release();
        return ERROR;
    }
    if (!gate.release()) return ERROR;
    local.release();
    if (before_cache_test_hook_) before_cache_test_hook_();
    txn_cache_.remember_shards(txn_id, ids);
    txn_cache_.remember(txn_id, mode, range, kind);
    return RCOK;
}

RC RemoteLockManager::release_impl(txnid_t txn_id, const KeyRangeEnt* range, LockKind kind) {
    if (!connected()) return ERROR;
    std::vector<std::size_t> ids = txn_cache_.shards(txn_id);
    if (ids.empty()) ids = range != nullptr ? shard_ids(*range) : shards_for_txn(txn_id);

    const auto queued_gate = profile_start(profile_enabled_);
    LocalGateGuard local(gate_mutexes_, ids, local_gate_enabled_);
    profile_add(profile_enabled_, queued_gate, local_gate_wait_ns_);
    RemoteGateGuard gate(*this);
    switch (gate.acquire(ids, true)) {
        case GateOutcome::Acquired: break;
        case GateOutcome::Busy: return AbortGate;
        case GateOutcome::TransportError: return ERROR;
    }

    if (after_gates_test_hook_) after_gates_test_hook_();
    auto& workspace = *current_slot().workspace;
    auto& tables = workspace.tables;
    if (!load_tables(ids, tables)) {
        gate.release();
        return ERROR;
    }

    const auto rkey = current_shards_rkey();
    const txnid_t id = wire_txn(txn_id);
    // One payload buffer per shard keeps every write request's source pointer
    // valid for the whole batch, since the transport copies at submit time.
    auto& moved = workspace.moved;
    auto& counts = workspace.counts;
    auto& writes = workspace.writes;
    writes.clear();
    for (auto& payload : moved) payload.clear();
    bool removed = false;
    for (std::size_t n = 0; n < ids.size(); ++n) {
        auto& table = tables[n];
        auto& slots = table.slots;
        const std::size_t original = slots.size();
        std::size_t i = 0;
        while (i < slots.size()) {
            const bool mine = owns_owner_token(slots[i].owner) && slots[i].txn_id == id;
            const bool matches = mine && (range == nullptr ||
                (slots[i].kind == static_cast<std::uint8_t>(kind) &&
                 same_range(decode_remote_slot(slots[i]).range, *range)));
            if (!matches) { ++i; continue; }
            removed = true;
            const std::size_t last = slots.size() - 1;
            if (i != last) {
                slots[i] = slots[last];
                moved[n].push_back(slots[i]);
                writes.push_back({shard_base(table.shard) + offsetof(RemoteLockShard, slots) +
                                      i * sizeof(RemoteLockSlot),
                                  rkey, &moved[n].back(), sizeof(RemoteLockSlot)});
            }
            slots.pop_back();
        }
        if (slots.size() != original) {
            counts[n] = slots.size();
            writes.push_back({shard_base(table.shard) + offsetof(RemoteLockShard, active_slots),
                              rkey, &counts[n], sizeof(std::uint64_t)});
        }
    }
    if (!write_chunks(writes)) {
        gate.release();
        return ERROR;
    }
    if (!gate.release()) return ERROR;
    local.release();

    if (range == nullptr) {
        // The lock cache is only valid while the transaction holds something;
        // leaving the entry behind makes every later release walk shards that
        // hold nothing of its own.
        txn_cache_.drop(txn_id);
    } else if (removed) {
        txn_cache_.forget(txn_id, *range, kind);
        refresh_txn_shards(txn_id);
    }
    return removed ? RCOK : Abort;
}

RC RemoteLockManager::unlock(txnid_t txn_id, const KeyRangeEnt& range, LockKind kind) {
    return release(txn_id, &range, kind);
}

RC RemoteLockManager::unlock_all(txnid_t txn_id) {
    return release(txn_id, nullptr, LockKind::Predicate);
}

void RemoteLockManager::record_count_error(RC rc) const {
    last_count_error_.store(rc, std::memory_order_relaxed);
    count_errors_.fetch_add(1, std::memory_order_release);
}

std::size_t RemoteLockManager::count_impl(txnid_t txn_id) const {
    if (!connected()) { record_count_error(ERROR); return 0; }
    if (txn_id != 0) {
        const auto cached = txn_cache_.lock_count(txn_id);
        if (cached != 0) return cached;
    }
    std::vector<std::size_t> ids;
    if (txn_id == 0) {
        ids.resize(kRemoteLockShards);
        for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = i;
    } else {
        ids = shards_for_txn(txn_id);
    }
    const txnid_t id = txn_id == 0 ? 0 : wire_txn(txn_id);
    const auto queued_gate = profile_start(profile_enabled_);
    LocalGateGuard local(gate_mutexes_, ids, local_gate_enabled_);
    profile_add(profile_enabled_, queued_gate, local_gate_wait_ns_);
    RemoteGateGuard gate(*this);
    const auto outcome = gate.acquire(ids, true);
    if (outcome != GateOutcome::Acquired) {
        record_count_error(outcome == GateOutcome::Busy ? AbortGate : ERROR);
        return 0;
    }
    if (after_gates_test_hook_) after_gates_test_hook_();
    auto& workspace = *current_slot().workspace;
    auto& tables = workspace.tables;
    if (!load_tables(ids, tables)) {
        gate.release();
        record_count_error(ERROR);
        return 0;
    }
    if (!gate.release()) { record_count_error(ERROR); return 0; }
    local.release();
    std::vector<RangeLock> unique;
    for (const auto& table : tables) {
        for (const auto& slot : table.slots) {
            if (txn_id != 0 && (!owns_owner_token(slot.owner) || slot.txn_id != id)) continue;
            const RangeLock lock = decode(slot);
            if (std::find_if(unique.begin(), unique.end(), [&](const RangeLock& other) {
                    return same_lock(lock, other);
                }) == unique.end()) unique.push_back(lock);
        }
    }
    return unique.size();
}

}  // namespace rrl
