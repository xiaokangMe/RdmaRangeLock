#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <infiniband/verbs.h>

namespace rrl {

struct RdmaRemoteMemory {
    std::uint64_t address{0};
    std::uint32_t rkey{0};
    std::uint64_t length{0};
};

// A small RC-QP transport. TCP is used only for exchanging QP and MR
// attributes; lock-table data moves through one-sided verbs operations.
class RdmaConnection {
public:
    RdmaConnection();
    ~RdmaConnection();
    RdmaConnection(const RdmaConnection&) = delete;
    RdmaConnection& operator=(const RdmaConnection&) = delete;

    bool connect_client(const std::string& host, std::uint16_t tcp_port);
    bool accept_server(std::uint16_t tcp_port, void* region, std::size_t region_length);
    bool read(std::uint64_t remote_address, std::uint32_t remote_rkey,
              void* destination, std::size_t length);
    bool write(std::uint64_t remote_address, std::uint32_t remote_rkey,
               const void* source, std::size_t length);
    bool compare_swap(std::uint64_t remote_address, std::uint32_t remote_rkey,
                      std::uint64_t compare, std::uint64_t swap,
                      std::uint64_t& previous);
    bool wait_for_disconnect();
    const RdmaRemoteMemory& remote_memory() const { return remote_memory_; }
    bool connected() const { return connected_; }

public:
    struct Impl;
private:
    Impl* impl_{nullptr};
    RdmaRemoteMemory remote_memory_{};
    bool connected_{false};
};

}  // namespace rrl