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

struct RdmaReadRequest {
    std::uint64_t remote_address{0};
    std::uint32_t remote_rkey{0};
    void* destination{nullptr};
    std::size_t length{0};
};

struct RdmaWriteRequest {
    std::uint64_t remote_address{0};
    std::uint32_t remote_rkey{0};
    const void* source{nullptr};
    std::size_t length{0};
};

struct RdmaCompareSwapRequest {
    std::uint64_t remote_address{0};
    std::uint32_t remote_rkey{0};
    std::uint64_t compare{0};
    std::uint64_t swap{0};
    std::uint64_t* previous{nullptr};
};

// A small RC-QP transport. TCP is used only for exchanging QP and MR
// attributes; lock-table data moves through one-sided verbs operations.
class RdmaConnection {
public:
    RdmaConnection();
    ~RdmaConnection();
    RdmaConnection(const RdmaConnection&) = delete;
    RdmaConnection& operator=(const RdmaConnection&) = delete;

    bool connect_client(const std::string& host, std::uint16_t tcp_port,
                        std::uint64_t owner_token = 0);
    bool accept_server(std::uint16_t tcp_port, void* region, std::size_t region_length);
    bool read(std::uint64_t remote_address, std::uint32_t remote_rkey,
              void* destination, std::size_t length);
    bool write(std::uint64_t remote_address, std::uint32_t remote_rkey,
               const void* source, std::size_t length);
    bool compare_swap(std::uint64_t remote_address, std::uint32_t remote_rkey,
                      std::uint64_t compare, std::uint64_t swap,
                      std::uint64_t& previous);
    bool read_batch(const std::vector<RdmaReadRequest>& requests);
    bool write_batch(const std::vector<RdmaWriteRequest>& requests);
    bool compare_swap_batch(const std::vector<RdmaCompareSwapRequest>& requests);
    bool wait_for_disconnect();
    std::uint64_t timeout_count() const;
    std::uint64_t completion_error_count() const;
    std::uint64_t operation_count() const;
    const RdmaRemoteMemory& remote_memory() const { return remote_memory_; }
    bool connected() const { return connected_; }
    std::uint64_t peer_owner_token() const;

public:
    struct Impl;
private:
    Impl* impl_{nullptr};
    RdmaRemoteMemory remote_memory_{};
    bool connected_{false};
};

}  // namespace rrl
