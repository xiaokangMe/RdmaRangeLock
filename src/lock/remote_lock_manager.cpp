#include "lock/remote_lock_manager.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <unordered_map>

namespace rrl {
namespace {

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

RemoteLockManager::RemoteLockManager() = default;
RemoteLockManager::~RemoteLockManager() = default;

struct RemoteLockManager::ConnectionSlot {
    std::unique_ptr<RdmaConnection> connection;
    std::uint64_t owner_token{0};
};

bool RemoteLockManager::connect(const std::string& host, std::uint16_t port) {
    if (connections_.size() != 0) return connected();
    if (const char* env = std::getenv("RRL_RDMA_BUCKET_WIDTH")) {
        char* end = nullptr;
        const auto parsed = std::strtoull(env, &end, 10);
        if (end && *end == '\0' && parsed != 0) bucket_width_ = parsed;
    }
    namespace_token_ = make_token(this, connection_count_) & 0x0000ffffffffffffULL;
    if (namespace_token_ == 0) namespace_token_ = 1;
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
            shards_rkey_ = remote.rkey;
        }
        connections_.push_back(std::move(slot));
    }
    next_connection_.store(0, std::memory_order_relaxed);
    ++connection_generation_;
    return connected();
}

void RemoteLockManager::set_bucket_width(std::uint64_t width) {
    if (width != 0) bucket_width_ = width;
}

void RemoteLockManager::set_connection_count(std::size_t count) {
    if (connections_.empty()) connection_count_ = std::max<std::size_t>(1, count);
}

bool RemoteLockManager::connected() const {
    if (connections_.empty()) return false;
    for (const auto& slot : connections_)
        if (!slot || !slot->connection || !slot->connection->connected()) return false;
    return true;
}

RemoteLockManager::ConnectionSlot& RemoteLockManager::current_slot() const {
    struct Assignment {
        std::size_t index{0};
        std::uint64_t generation{0};
    };
    static thread_local std::unordered_map<const RemoteLockManager*, Assignment> assignments;
    auto it = assignments.find(this);
    if (it == assignments.end() || it->second.generation != connection_generation_) {
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

std::uint64_t RemoteLockManager::shard_base(std::size_t shard) const {
    return shards_address_ + shard * sizeof(RemoteLockShard);
}

bool RemoteLockManager::enter_shards(const std::vector<std::size_t>& ids,
                                     std::vector<std::size_t>& acquired) const {
    acquired.clear();
    auto& connection = current_connection();
    const auto owner = current_owner_token();
    for (std::size_t shard : ids) {
        std::uint64_t previous = 0;
        const auto gate = shard_base(shard) + offsetof(RemoteLockShard, gate);
        if (!connection.compare_swap(gate, shards_rkey_, 0, owner, previous) || previous != 0) {
            leave_shards(acquired);
            return false;
        }
        acquired.push_back(shard);
    }
    return true;
}

void RemoteLockManager::leave_shards(const std::vector<std::size_t>& acquired) const {
    auto& connection = current_connection();
    const auto owner = current_owner_token();
    for (auto it = acquired.rbegin(); it != acquired.rend(); ++it) {
        std::uint64_t expected = owner;
        const auto gate = shard_base(*it) + offsetof(RemoteLockShard, gate);
        (void)connection.compare_swap(gate, shards_rkey_, owner, 0, expected);
    }
}

bool RemoteLockManager::load_slots(std::size_t shard,
                                   std::vector<RemoteLockSlot>& slots) const {
    auto& connection = current_connection();
    const auto base = shard_base(shard);
    std::uint64_t count = 0;
    if (!connection.read(base + offsetof(RemoteLockShard, active_slots), shards_rkey_,
                         &count, sizeof(count)) || count > kRemoteLockSlotsPerShard)
        return false;
    slots.resize(static_cast<std::size_t>(count));
    if (slots.empty()) return true;
    return connection.read(base + offsetof(RemoteLockShard, slots), shards_rkey_,
                           slots.data(), sizeof(RemoteLockSlot) * slots.size());
}

bool RemoteLockManager::store_active_slots(std::size_t shard, std::uint64_t count) const {
    if (count > kRemoteLockSlotsPerShard) return false;
    auto& connection = current_connection();
    const auto base = shard_base(shard);
    return connection.write(base + offsetof(RemoteLockShard, active_slots), shards_rkey_,
                            &count, sizeof(count));
}

RangeLock RemoteLockManager::decode(const RemoteLockSlot& slot) {
    KeyRangeEnt range;
    range.StartKey = slot.start_key;
    range.EndKey = slot.end_key;
    range.has_start = slot.has_start != 0;
    range.has_end = slot.has_end != 0;
    range.start_bound = slot.start_bound ? Bound::Closed : Bound::Open;
    range.end_bound = slot.end_bound ? Bound::Closed : Bound::Open;
    return RangeLock{slot.txn_id, static_cast<LockMode>(slot.mode),
                     static_cast<LockKind>(slot.kind), range};
}

void RemoteLockManager::encode(RemoteLockSlot& slot, std::uint64_t owner,
                               txnid_t txn_id, LockMode mode, LockKind kind,
                               const KeyRangeEnt& range) {
    slot = RemoteLockSlot{};
    slot.owner = owner;
    slot.txn_id = txn_id;
    slot.start_key = range.StartKey;
    slot.end_key = range.EndKey;
    slot.mode = static_cast<std::uint8_t>(mode);
    slot.kind = static_cast<std::uint8_t>(kind);
    slot.has_start = range.has_start ? 1 : 0;
    slot.has_end = range.has_end ? 1 : 0;
    slot.start_bound = range.start_bound == Bound::Closed ? 1 : 0;
    slot.end_bound = range.end_bound == Bound::Closed ? 1 : 0;
}

txnid_t RemoteLockManager::wire_txn(txnid_t txn_id) const {
    return (current_namespace_token() << 16) ^ (txn_id & 0x0000ffffffffffffULL);
}

RC RemoteLockManager::lock(txnid_t txn_id, LockMode mode, const KeyRangeEnt& range,
                           LockKind kind, std::vector<txnid_t>* conflicts) {
    if (!connected() || range.empty() || !range.valid()) return ERROR;
    if (conflicts) conflicts->clear();
    const auto ids = shard_ids(range);
    std::vector<std::size_t> acquired;
    if (!enter_shards(ids, acquired)) return Abort;
    auto& connection = current_connection();
    const auto owner = current_owner_token();
    std::vector<std::vector<RemoteLockSlot>> tables(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (!load_slots(ids[i], tables[i])) { leave_shards(acquired); return ERROR; }
    }
    const txnid_t id = wire_txn(txn_id);
    RangeLock request{id, mode, kind, range};
    bool conflict = false;
    for (const auto& table : tables) {
        for (const auto& slot : table) {
            const RangeLock held = decode(slot);
            if (!request.conflicts(held)) continue;
            conflict = true;
            if (conflicts && std::find(conflicts->begin(), conflicts->end(), held.txn_id) == conflicts->end())
                conflicts->push_back(held.txn_id);
        }
    }
    if (conflict) { leave_shards(acquired); return Abort; }
    for (std::size_t n = 0; n < ids.size(); ++n) {
        auto& table = tables[n];
        bool found = false;
        for (std::size_t i = 0; i < table.size(); ++i) {
            const RangeLock held = decode(table[i]);
            if (!owns_owner_token(table[i].owner) || table[i].txn_id != id ||
                held.kind != kind || !same_range(held.range, range)) continue;
            table[i].mode = static_cast<std::uint8_t>(lock_mode_join(held.mode, mode));
            const auto address = shard_base(ids[n]) + offsetof(RemoteLockShard, slots) +
                                 i * sizeof(RemoteLockSlot);
            if (!connection.write(address, shards_rkey_, &table[i], sizeof(RemoteLockSlot))) {
                leave_shards(acquired); return ERROR;
            }
            found = true;
            break;
        }
        if (found) continue;
        if (table.size() == kRemoteLockSlotsPerShard) { leave_shards(acquired); return Abort; }
        RemoteLockSlot slot{};
        encode(slot, owner, id, mode, kind, range);
        const auto address = shard_base(ids[n]) + offsetof(RemoteLockShard, slots) +
                             table.size() * sizeof(RemoteLockSlot);
        if (!connection.write(address, shards_rkey_, &slot, sizeof(slot)) ||
            !store_active_slots(ids[n], table.size() + 1)) {
            leave_shards(acquired); return ERROR;
        }
        table.push_back(slot);
    }
    leave_shards(acquired);
    return RCOK;
}

RC RemoteLockManager::unlock(txnid_t txn_id, const KeyRangeEnt& range, LockKind kind) {
    if (!connected()) return Abort;
    const auto ids = shard_ids(range);
    std::vector<std::size_t> acquired;
    if (!enter_shards(ids, acquired)) return Abort;
    auto& connection = current_connection();
    const txnid_t id = wire_txn(txn_id);
    bool removed = false;
    for (std::size_t shard : ids) {
        std::vector<RemoteLockSlot> table;
        if (!load_slots(shard, table)) { leave_shards(acquired); return ERROR; }
        std::size_t i = 0;
        while (i < table.size()) {
            const RangeLock held = decode(table[i]);
            if (!owns_owner_token(table[i].owner) || table[i].txn_id != id ||
                held.kind != kind || !same_range(held.range, range)) { ++i; continue; }
            removed = true;
            const std::size_t last = table.size() - 1;
            if (i != last) {
                table[i] = table[last];
                const auto address = shard_base(shard) + offsetof(RemoteLockShard, slots) +
                                     i * sizeof(RemoteLockSlot);
                if (!connection.write(address, shards_rkey_, &table[i], sizeof(RemoteLockSlot))) {
                    leave_shards(acquired); return ERROR;
                }
            }
            table.pop_back();
        }
        if (!store_active_slots(shard, table.size())) { leave_shards(acquired); return ERROR; }
    }
    leave_shards(acquired);
    return removed ? RCOK : Abort;
}

RC RemoteLockManager::unlock_all(txnid_t txn_id) {
    if (!connected()) return Abort;
    std::vector<std::size_t> ids(kRemoteLockShards);
    for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = i;
    std::vector<std::size_t> acquired;
    if (!enter_shards(ids, acquired)) return Abort;
    auto& connection = current_connection();
    const txnid_t id = wire_txn(txn_id);
    for (std::size_t shard : ids) {
        std::vector<RemoteLockSlot> table;
        if (!load_slots(shard, table)) { leave_shards(acquired); return ERROR; }
        std::size_t i = 0;
        while (i < table.size()) {
            if (!owns_owner_token(table[i].owner) || table[i].txn_id != id) { ++i; continue; }
            const std::size_t last = table.size() - 1;
            if (i != last) {
                table[i] = table[last];
                const auto address = shard_base(shard) + offsetof(RemoteLockShard, slots) +
                                     i * sizeof(RemoteLockSlot);
                if (!connection.write(address, shards_rkey_, &table[i], sizeof(RemoteLockSlot))) {
                    leave_shards(acquired); return ERROR;
                }
            }
            table.pop_back();
        }
        if (!store_active_slots(shard, table.size())) { leave_shards(acquired); return ERROR; }
    }
    leave_shards(acquired);
    return RCOK;
}

std::size_t RemoteLockManager::get_lock_count(txnid_t txn_id) const {
    if (!connected()) return 0;
    std::vector<std::size_t> ids(kRemoteLockShards);
    for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = i;
    std::vector<std::size_t> acquired;
    if (!enter_shards(ids, acquired)) return 0;
    const txnid_t id = txn_id == 0 ? 0 : wire_txn(txn_id);
    std::vector<RangeLock> unique;
    for (std::size_t shard : ids) {
        std::vector<RemoteLockSlot> table;
        if (!load_slots(shard, table)) { leave_shards(acquired); return 0; }
        for (const auto& slot : table) {
            if (txn_id != 0 && (!owns_owner_token(slot.owner) || slot.txn_id != id)) continue;
            const RangeLock lock = decode(slot);
            if (std::find_if(unique.begin(), unique.end(), [&](const RangeLock& other) {
                    return same_lock(lock, other);
                }) == unique.end()) unique.push_back(lock);
        }
    }
    leave_shards(acquired);
    return unique.size();
}

}  // namespace rrl
