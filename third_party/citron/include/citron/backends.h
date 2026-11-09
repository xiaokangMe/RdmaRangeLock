// Concrete backends: RDMA (one-sided verbs) and a local atomic array used by
// protocol tests and as a NIC-free fallback.
#pragma once

#include "citron/backend.h"
#include "rdma/rdma_transport.h"

#include <cstdint>
#include <memory>
#include <string>

namespace citron {

class LocalBackend final : public Backend {
public:
    LocalBackend(std::uint64_t* words, std::size_t node_count);
    bool read_node(std::uint64_t index, Node& out) override;
    bool cas(std::uint64_t index, int field, std::uint64_t expected,
             std::uint64_t desired, std::uint64_t& previous) override;
    bool faa(std::uint64_t index, int field, std::uint64_t add,
             std::uint64_t& previous) override;
    std::uint64_t operations() const override { return operations_; }

private:
    std::uint64_t* word(std::uint64_t index, int field) const;
    std::uint64_t* words_;
    std::size_t node_count_;
    mutable std::uint64_t operations_{0};
};

class RdmaBackend final : public Backend {
public:
    RdmaBackend(const std::string& host, std::uint16_t port);
    bool read_node(std::uint64_t index, Node& out) override;
    bool read_node_many(const std::vector<std::uint64_t>& indices,
                        std::vector<Node>& out) override;
    bool read_node_range(std::uint64_t first_index, std::size_t count,
                         std::vector<Node>& out) override;
    bool cas(std::uint64_t index, int field, std::uint64_t expected,
             std::uint64_t desired, std::uint64_t& previous) override;
    bool faa(std::uint64_t index, int field, std::uint64_t add,
             std::uint64_t& previous) override;
    bool faa_many(const std::vector<FaaOp>& ops) override;
    std::uint64_t operations() const override { return connection_.operation_count(); }
    std::uint64_t transport_failures() const override {
        return connection_.failure_count();
    }
    const rrl::RdmaRemoteMemory& region() const { return connection_.remote_memory(); }

private:
    std::uint64_t offset(std::uint64_t index, int field) const;
    rrl::RdmaConnection connection_;
};

}  // namespace citron
