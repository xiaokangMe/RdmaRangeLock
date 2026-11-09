#pragma once
#include "index/btree.h"
#include "rdma/rdma_transport.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rolex::remote {

constexpr std::uint64_t kMagic = 0x52524c524f4c4558ULL; // RRLROLEX
constexpr std::uint64_t kFormat = 1;
constexpr std::size_t kLeafSlots = 64;

struct RemoteRolexHeader {
    std::uint64_t magic{kMagic};
    std::uint64_t format{kFormat};
    std::uint64_t bytes{0};
    std::uint64_t leaf_slots{kLeafSlots};
    std::uint64_t leaf_capacity{0};
    std::uint64_t active_leaves{0};
    std::uint64_t next_leaf{0};
    std::uint64_t live_keys{0};
    std::uint64_t directory_version{0};
    std::uint64_t directory_lock{0};
    std::uint64_t seed_fingerprint{0};
    std::uint64_t directory_offset{0};
    std::uint64_t leaves_offset{0};
    std::uint64_t leaf_bytes{0};
    std::uint64_t reserved[2]{};
};
static_assert(sizeof(RemoteRolexHeader) == 128, "stable remote ROLEX header");

struct RemoteRolexDirectoryEntry {
    std::uint64_t first_key{0};
    std::uint64_t last_key{0};
    std::uint64_t leaf_number{0};
    std::uint64_t synonym{0};
};
static_assert(sizeof(RemoteRolexDirectoryEntry) == 32, "stable remote ROLEX directory");

struct RemoteRolexLeaf {
    std::uint64_t version{0}; // even=stable, odd=writer owns the leaf
    std::uint64_t used{0};
    std::uint64_t next{0};
    std::uint64_t prev{0};
    std::uint64_t keys[kLeafSlots]{};
    std::uint64_t values[kLeafSlots]{};
};
static_assert(sizeof(RemoteRolexLeaf) == 1056, "stable remote ROLEX leaf");

struct RemoteRolexConfig {
    std::size_t leaf_capacity{4096};
    std::size_t leaf_slots{kLeafSlots};
};

using Seed = std::vector<std::pair<rrl::idx_key_t, std::uint64_t>>;

std::uint64_t seed_fingerprint(const Seed&);
std::size_t region_bytes(const RemoteRolexConfig&);
class RemoteRolexMemory {
public:
    RemoteRolexMemory(const RemoteRolexConfig&, const Seed&);
    ~RemoteRolexMemory();
    void* data();
    std::size_t bytes() const;
public:
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

class RolexRdmaIndex final : public rrl::BTreeIndex {
public:
    RolexRdmaIndex();
    ~RolexRdmaIndex() override;
    RolexRdmaIndex(const RolexRdmaIndex&) = delete;
    RolexRdmaIndex& operator=(const RolexRdmaIndex&) = delete;

    bool connect(const std::string&, std::uint16_t, std::size_t,
                 std::uint64_t expected_seed, std::uint64_t timeout_ms=10000);
    rrl::RC init(std::size_t order) override;
    rrl::RC insert(rrl::idx_key_t, void*) override;
    rrl::RC erase(rrl::idx_key_t) override;
    bool lookup(rrl::idx_key_t, void*&) const override;
    std::vector<rrl::KeyValue> scan_range(const rrl::KeyRangeEnt&) const override;
    void scan_range_into(const rrl::KeyRangeEnt&, std::vector<rrl::KeyValue>&) const override;
    std::size_t size() const override;
    bool preloaded() const override { return true; }
    void prepare_thread(std::size_t, std::size_t) override;
    void finish_thread() noexcept override;
    void prepare_write_capacity(std::size_t) override;
    bool healthy() const override;
    rrl::RC apply_batch(const std::vector<rrl::IndexMutation>&) override;
    bool key_neighbors(rrl::idx_key_t, rrl::IndexKeyNeighbors&) const override;
    bool key_range_into(const rrl::KeyRangeEnt&, std::vector<rrl::idx_key_t>&) const override;
    bool validate() const;
    std::uint64_t rdma_operations() const;
    std::uint64_t read_bytes() const;
    std::uint64_t write_bytes() const;
public:
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

} // namespace rolex::remote
