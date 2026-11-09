// Storage backend for the lock tree, expressed in the node layout above.
// All operations are single one-sided RDMA verbs on 8-byte aligned words.
#pragma once

#include "citron/node_format.h"

#include <cstdint>
#include <vector>

namespace citron {

class Backend {
public:
    struct FaaOp {
        std::uint64_t index{0};
        int field{0};
        std::uint64_t add{0};
    };

    virtual ~Backend() = default;

    virtual bool read_node(std::uint64_t index, Node& out) = 0;
    virtual bool read_node_many(const std::vector<std::uint64_t>& indices,
                                std::vector<Node>& out);
    virtual bool read_node_range(std::uint64_t first_index, std::size_t count,
                                 std::vector<Node>& out);
    // Returns true when the verb completed; `previous` is the value before the
    // swap. The caller compares `previous` with `expected`.
    virtual bool cas(std::uint64_t index, int field, std::uint64_t expected,
                     std::uint64_t desired, std::uint64_t& previous) = 0;
    virtual bool faa(std::uint64_t index, int field, std::uint64_t add,
                     std::uint64_t& previous) = 0;
    virtual bool faa_many(const std::vector<FaaOp>& ops);
    virtual std::uint64_t operations() const = 0;
    virtual std::uint64_t transport_failures() const { return 0; }
};

// Node memory geometry shared by server, local backend and validation.
constexpr std::uint64_t kNodeBytes = kNodeWords * 8;
inline std::uint64_t region_bytes(std::uint64_t nodes) {
    return nodes * kNodeBytes;
}

}  // namespace citron
