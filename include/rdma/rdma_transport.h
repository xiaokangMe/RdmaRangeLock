#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <infiniband/verbs.h>

namespace rrl {

// Phase tag attached to a work request so a timeout can be attributed to the
// step that issued it.
enum class RdmaOpTag : std::uint8_t {
    None = 0,
    Gate = 1,
    LoadCounts = 2,
    LoadSlots = 3,
    StoreSlot = 4,
    ReleaseGate = 5,
};

constexpr std::uint32_t kRdmaUnknownWr = UINT32_MAX;

// Everything needed to explain a single failed work request. Unknown request
// identity (poll error/timeout) uses kRdmaUnknownWr and zero address/length.
struct RdmaFailureInfo {
    std::uint32_t opcode{0};
    std::uint32_t status{0};
    std::uint32_t wr_index{0};
    std::uint32_t length{0};
    std::uint64_t remote_address{0};
    std::uint64_t elapsed_us{0};
    std::uint64_t qp_num{0};
    // Process-wide publication sequence, comparable across connections.
    std::uint64_t sequence{0};
    std::uint8_t tag{0};
    std::uint8_t reserved[7]{};
};

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

// Keep the listening socket alive across handshakes; closing it after each
// accept discards other queued control connections during disconnect recovery.
class RdmaListener {
public:
    explicit RdmaListener(std::uint16_t port);
    ~RdmaListener();
    RdmaListener(const RdmaListener&) = delete;
    RdmaListener& operator=(const RdmaListener&) = delete;
    bool valid() const { return fd_ >= 0; }
private:
    friend class RdmaConnection;
    int fd_{-1};
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
    bool accept_server(RdmaListener& listener, void* region, std::size_t region_length);
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
    // Additional absolute budget, used for the complete server recovery.
    void set_deadline(std::chrono::steady_clock::time_point deadline);
    // After accept_server() returns false, a malformed/timed-out handshake may
    // be rejected without stopping the listener. Resource/listener errors are fatal.
    bool rejected_handshake() const;
    std::uint64_t timeout_count() const;
    std::uint64_t timeout_count(RdmaOpTag tag) const;
    std::uint64_t completion_error_count() const;
    std::uint64_t operation_count() const;
    // Tagging is per connection and therefore per owning thread; it lets the
    // manager record which protocol step issued the request that stalled.
    void set_operation_tag(RdmaOpTag tag);
    // Snapshot, not a reference: the record is overwritten by later failures.
    RdmaFailureInfo last_failure() const;
    std::uint64_t failure_count() const;
    const RdmaRemoteMemory& remote_memory() const { return remote_memory_; }
    bool connected() const;
    // Valid only after wait_for_disconnect(): None means an unannounced close.
    RdmaOpTag peer_failure_tag() const;
    std::uint64_t peer_owner_token() const;

public:
    struct Impl;
private:
    Impl* impl_{nullptr};
    RdmaRemoteMemory remote_memory_{};
    bool connected_{false};
};

}  // namespace rrl
