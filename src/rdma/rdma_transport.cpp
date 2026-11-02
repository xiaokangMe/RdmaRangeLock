#include "rdma/rdma_transport.h"
#include "common/config_parse.h"
#include "control_io.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <cstdlib>
#include <thread>
#include <vector>

namespace rrl {
namespace {

constexpr std::size_t kScratchBytes = 1u << 20;
constexpr std::uint8_t kPort = 1;
std::atomic<std::uint64_t> next_failure_sequence{1};

int gid_index() {
    const char* value = std::getenv("RRL_RDMA_GID_INDEX");
    if (!value) return 0;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return end && *end == '\0' && parsed >= 0 ? static_cast<int>(parsed) : 0;
}

struct WireInfo {
    std::uint32_t qpn{0};
    std::uint32_t psn{0};
    std::uint16_t lid{0};
    std::uint8_t gid[16]{};
    std::uint64_t address{0};
    std::uint32_t rkey{0};
    std::uint64_t length{0};
    std::uint64_t owner_token{0};
};

using control::send_all;
using control::recv_all;
using Clock = std::chrono::steady_clock;
Clock::time_point handshake_deadline(Clock::time_point overall) {
    const char* value = std::getenv("RRL_RDMA_CONTROL_TIMEOUT_MS");
    const auto ms = value ? parse_unsigned(value, "RRL_RDMA_CONTROL_TIMEOUT_MS") : 3000;
    if (ms == 0 || ms > 600000) throw std::invalid_argument("control timeout must be 1..600000 ms");
    return std::min(overall, Clock::now() + std::chrono::milliseconds(ms));
}

int make_listen_socket(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, SOMAXCONN) != 0) {
        ::close(fd); return -1;
    }
    return fd;
}

void enable_keepalive(int fd) {
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int idle = 5;
    int interval = 2;
    int count = 3;
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
}

int make_client_socket(const std::string& host, std::uint16_t port, Clock::time_point deadline) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd); return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (errno != EINPROGRESS || !control::wait_fd(fd, POLLOUT, deadline)) {
            ::close(fd); return -1;
        }
        int error = 0;
        socklen_t size = sizeof(error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0) {
            ::close(fd); return -1;
        }
    }
    enable_keepalive(fd);
    return fd;
}

bool query_local(RdmaConnection::Impl* impl, WireInfo& wire);
bool transition_init(RdmaConnection::Impl* impl);
bool transition_ready(RdmaConnection::Impl* impl, const WireInfo& remote);

}  // namespace

struct RdmaConnection::Impl {
    ibv_context* context{nullptr};
    ibv_pd* pd{nullptr};
    ibv_cq* cq{nullptr};
    ibv_qp* qp{nullptr};
    ibv_mr* local_mr{nullptr};
    ibv_mr* region_mr{nullptr};
    std::vector<std::uint8_t> scratch;
    int socket_fd{-1};
    std::uint32_t psn{0};
    std::uint64_t peer_owner_token{0};
    std::atomic<std::uint64_t> rdma_timeouts{0};
    std::atomic<std::uint64_t> completion_errors{0};
    std::atomic<std::uint64_t> operations{0};
    std::atomic<std::uint64_t> failures{0};
    std::atomic<std::uint64_t> timeouts_by_tag[8];
    std::uint8_t op_tag{0};
    RdmaFailureInfo last_failure{};
    mutable std::mutex failure_mutex;
    std::atomic<bool> failed{false};
    RdmaOpTag peer_failure_tag{RdmaOpTag::None};
    Clock::time_point deadline{Clock::time_point::max()};
    bool rejected_handshake{false};

    Impl() {
        for (auto& counter : timeouts_by_tag) counter.store(0, std::memory_order_relaxed);
    }

    ~Impl() {
        if (qp && ibv_destroy_qp(qp) != 0) std::abort();
        if (region_mr) ibv_dereg_mr(region_mr);
        if (local_mr) ibv_dereg_mr(local_mr);
        if (cq) ibv_destroy_cq(cq);
        if (pd) ibv_dealloc_pd(pd);
        if (context) ibv_close_device(context);
        if (socket_fd >= 0) ::close(socket_fd);
    }
};

namespace {

bool create_resources(RdmaConnection::Impl* impl, void* region, std::size_t region_length,
                     bool client) {
    int count = 0;
    ibv_device** devices = ibv_get_device_list(&count);
    if (!devices || count == 0) return false;
    const char* requested = std::getenv("RRL_RDMA_DEVICE");
    for (int i = 0; i < count && !impl->context; ++i) {
        if (requested && std::string(ibv_get_device_name(devices[i])) != requested) continue;
        ibv_context* candidate = ibv_open_device(devices[i]);
        if (!candidate) continue;
        ibv_port_attr port{};
        if (ibv_query_port(candidate, kPort, &port) == 0 && port.state == IBV_PORT_ACTIVE)
            impl->context = candidate;
        else
            ibv_close_device(candidate);
    }
    ibv_free_device_list(devices);
    if (!impl->context) return false;
    impl->pd = ibv_alloc_pd(impl->context);
    impl->cq = ibv_create_cq(impl->context, 64, nullptr, nullptr, 0);
    if (!impl->pd || !impl->cq) return false;

    ibv_qp_init_attr init{};
    init.send_cq = impl->cq;
    init.recv_cq = impl->cq;
    init.qp_type = IBV_QPT_RC;
    init.cap.max_send_wr = 256;
    init.cap.max_recv_wr = 1;
    init.cap.max_send_sge = 1;
    init.cap.max_recv_sge = 1;
    impl->qp = ibv_create_qp(impl->pd, &init);
    if (!impl->qp) return false;

    if (client) {
        impl->scratch.resize(kScratchBytes);
        impl->local_mr = ibv_reg_mr(impl->pd, impl->scratch.data(), impl->scratch.size(),
                                     IBV_ACCESS_LOCAL_WRITE);
        if (!impl->local_mr) return false;
    } else {
        impl->region_mr = ibv_reg_mr(impl->pd, region, region_length,
                                      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                                      IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC);
        if (!impl->region_mr) return false;
    }
    impl->psn = static_cast<std::uint32_t>(::getpid() ^ reinterpret_cast<std::uintptr_t>(impl)) & 0x00ffffffu;
    return transition_init(impl);
}

bool query_local(RdmaConnection::Impl* impl, WireInfo& wire) {
    ibv_port_attr port{};
    if (ibv_query_port(impl->context, kPort, &port) != 0) return false;
    wire.qpn = impl->qp->qp_num;
    wire.psn = impl->psn;
    wire.lid = port.lid;
    ibv_gid gid{};
    if (ibv_query_gid(impl->context, kPort, gid_index(), &gid) != 0) return false;
    std::memcpy(wire.gid, gid.raw, sizeof(wire.gid));
    if (impl->region_mr) {
        wire.address = reinterpret_cast<std::uint64_t>(impl->region_mr->addr);
        wire.rkey = impl->region_mr->rkey;
    }
    return true;
}

bool transition_init(RdmaConnection::Impl* impl) {
    ibv_qp_attr attr{};
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = kPort;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_ATOMIC;
    return ibv_modify_qp(impl->qp, &attr,
                         IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                         IBV_QP_ACCESS_FLAGS) == 0;
}

bool transition_ready(RdmaConnection::Impl* impl, const WireInfo& remote) {
    ibv_qp_attr attr{};
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = remote.qpn;
    attr.rq_psn = remote.psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.dlid = remote.lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = kPort;
    std::memcpy(&attr.ah_attr.grh.dgid, remote.gid, sizeof(remote.gid));
    attr.ah_attr.grh.flow_label = 0;
    attr.ah_attr.grh.sgid_index = gid_index();
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.grh.traffic_class = 0;
    if (ibv_modify_qp(impl->qp, &attr,
                      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                      IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) != 0) return false;

    std::memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = impl->psn;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.max_rd_atomic = 1;
    return ibv_modify_qp(impl->qp, &attr,
                          IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                          IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                          IBV_QP_MAX_QP_RD_ATOMIC) == 0;
}

std::uint64_t wr_remote_address(const ibv_send_wr& wr) {
    return wr.wr.rdma.remote_addr;
}

// Records everything needed to explain one failed request. The phase tag is set
// by the caller (lock manager) before it issues a request, so a timeout can be
// attributed to the gate, the table read, the slot write or the release.
void record_failure(RdmaConnection::Impl* impl, std::uint32_t status, std::uint32_t opcode,
                    std::uint64_t remote_address, std::uint32_t length, std::uint32_t wr_index,
                    std::chrono::steady_clock::time_point started, bool timeout) {
    if (timeout) impl->rdma_timeouts.fetch_add(1, std::memory_order_relaxed);
    else impl->completion_errors.fetch_add(1, std::memory_order_relaxed);
    const std::size_t tag = std::min<std::size_t>(impl->op_tag, 7);
    if (timeout) impl->timeouts_by_tag[tag].fetch_add(1, std::memory_order_relaxed);
    impl->failures.fetch_add(1, std::memory_order_relaxed);
    RdmaFailureInfo info;
    info.opcode = opcode;
    info.status = status;
    info.wr_index = wr_index;
    info.length = length;
    info.remote_address = remote_address;
    info.elapsed_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
    info.qp_num = impl->qp ? impl->qp->qp_num : 0;
    info.tag = impl->op_tag;
    std::lock_guard<std::mutex> guard(impl->failure_mutex);
    info.sequence = next_failure_sequence.fetch_add(1, std::memory_order_relaxed);
    impl->last_failure = info;
}

// A failed completion leaves ownership uncertain. Quiesce the QP before
// notifying the peer; recovery must never race a late WRITE/CAS from this QP.
void fail_connection(RdmaConnection::Impl* impl) {
    impl->failed.store(true, std::memory_order_release);
    if (impl->qp) {
        if (ibv_destroy_qp(impl->qp) != 0) std::abort();
        impl->qp = nullptr;
    }
    if (impl->socket_fd >= 0) {
        const std::uint8_t notice = 0x80u | impl->op_tag;
        // Failure notification must not block the caller holding local gates.
        // A lost notice is treated conservatively as an unannounced crash.
        (void)::send(impl->socket_fd, &notice, sizeof(notice), MSG_NOSIGNAL | MSG_DONTWAIT);
        ::shutdown(impl->socket_fd, SHUT_RDWR);
    }
}

bool post_and_poll(RdmaConnection::Impl* impl, ibv_send_wr& wr, ibv_sge& sge) {
    wr.wr_id = 1;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;
    ibv_send_wr* bad = nullptr;
    const auto started = std::chrono::steady_clock::now();
    if (started >= impl->deadline) {
        record_failure(impl, 0xfffffffdu, wr.opcode, wr_remote_address(wr), sge.length, 0, started, true);
        fail_connection(impl); return false;
    }
    if (ibv_post_send(impl->qp, &wr, &bad) != 0) {
        record_failure(impl, 0xffffffffu, wr.opcode, wr_remote_address(wr), sge.length, 0, started, false);
        fail_connection(impl);
        return false;
    }
    impl->operations.fetch_add(1, std::memory_order_relaxed);
    ibv_wc wc{};
    const auto deadline = std::min(impl->deadline, started + std::chrono::seconds(1));
    for (;;) {
        const int polled = ibv_poll_cq(impl->cq, 1, &wc);
        if (polled < 0) {
            record_failure(impl, 0xfffffffeu, wr.opcode, wr_remote_address(wr), sge.length, 0, started, false);
            fail_connection(impl);
            return false;
        }
        if (polled > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                record_failure(impl, static_cast<std::uint32_t>(wc.status), wr.opcode,
                               wr_remote_address(wr), sge.length, 0, started, false);
                fail_connection(impl);
            }
            return wc.status == IBV_WC_SUCCESS;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            record_failure(impl, 0xfffffffdu, wr.opcode, wr_remote_address(wr), sge.length, 0, started, true);
            fail_connection(impl);
            return false;
        }
        std::this_thread::yield();
    }
}

void record_batch_failure(RdmaConnection::Impl* impl, std::uint32_t status,
                          const std::vector<ibv_send_wr>& wrs,
                          const std::vector<ibv_sge>& sges, std::size_t index,
                          Clock::time_point started, bool timeout) {
    if (index < wrs.size())
        record_failure(impl, status, wrs[index].opcode, wr_remote_address(wrs[index]),
                       sges[index].length, static_cast<std::uint32_t>(index), started, timeout);
    else
        record_failure(impl, status, wrs.front().opcode, 0, 0, kRdmaUnknownWr, started, timeout);
}

bool post_batch_and_poll(RdmaConnection::Impl* impl,
                         std::vector<ibv_send_wr>& wrs,
                         std::vector<ibv_sge>& sges) {
    if (wrs.empty() || wrs.size() != sges.size() || wrs.size() > 128) return false;
    for (std::size_t i = 0; i < wrs.size(); ++i) {
        wrs[i].wr_id = i + 1;
        wrs[i].sg_list = &sges[i];
        wrs[i].num_sge = 1;
        // Fence the signaled tail behind prior READ/atomic responses before
        // reusing their scratch segments. Do not rely on NIC issue ordering
        // alone as a completion barrier for earlier unsignaled operations.
        wrs[i].send_flags = i + 1 == wrs.size() ? (IBV_SEND_SIGNALED | IBV_SEND_FENCE) : 0;
        wrs[i].next = i + 1 == wrs.size() ? nullptr : &wrs[i + 1];
    }
    ibv_send_wr* bad = nullptr;
    const auto started = std::chrono::steady_clock::now();
    if (started >= impl->deadline) {
        record_batch_failure(impl, 0xfffffffdu, wrs, sges, kRdmaUnknownWr, started, true);
        fail_connection(impl); return false;
    }
    if (ibv_post_send(impl->qp, &wrs.front(), &bad) != 0) {
        // A prefix may already be in flight without a signaled completion.
        // Never reuse its registered buffer on this QP after partial posting.
        std::size_t posted = 0;
        while (posted < wrs.size() && &wrs[posted] != bad) ++posted;
        impl->operations.fetch_add(posted, std::memory_order_relaxed);
        record_batch_failure(impl, 0xffffffffu, wrs, sges, posted, started, false);
        fail_connection(impl);
        return false;
    }
    impl->operations.fetch_add(wrs.size(), std::memory_order_relaxed);
    ibv_wc wc{};
    const auto deadline = std::min(impl->deadline, started + std::chrono::seconds(1));
    for (;;) {
        const int polled = ibv_poll_cq(impl->cq, 1, &wc);
        if (polled < 0) {
            record_batch_failure(impl, 0xfffffffeu, wrs, sges, kRdmaUnknownWr, started, false);
            fail_connection(impl);
            return false;
        }
        if (polled > 0) {
            if (wc.status != IBV_WC_SUCCESS || wc.wr_id != wrs.size()) {
                const auto index = wc.wr_id > 0 && wc.wr_id <= wrs.size() ? wc.wr_id - 1 : kRdmaUnknownWr;
                const auto status = wc.status == IBV_WC_SUCCESS ? 0xfffffffcu : static_cast<std::uint32_t>(wc.status);
                record_batch_failure(impl, status, wrs, sges, index, started, false);
                fail_connection(impl);
                return false;
            }
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            record_batch_failure(impl, 0xfffffffdu, wrs, sges, kRdmaUnknownWr, started, true);
            fail_connection(impl);
            return false;
        }
        std::this_thread::yield();
    }
}

// A single work request is one segment over the registered scratch buffer, so
// the parameters must satisfy three things: a usable host buffer, a length that
// fits both that buffer and the 32-bit SGE length field, and a plausible remote
// address. read_batch/write_batch already refused malformed requests while the
// single-request entry points did not: an oversized length silently truncated
// into sge.length and then memcpy'd past the scratch buffer.
bool valid_transfer(const void* buffer, std::size_t length, std::size_t capacity) {
    return buffer != nullptr && length != 0 && length <= capacity &&
           length <= std::numeric_limits<std::uint32_t>::max();
}

// RDMA atomics are only defined on naturally aligned remote addresses; an
// unaligned one comes back as IBV_WC_REM_INV_REQ_OPCODE several round trips
// later, which is far harder to attribute than refusing it here.
bool valid_atomic_target(std::uint64_t remote_address) {
    return remote_address != 0 && (remote_address % sizeof(std::uint64_t)) == 0;
}

}  // namespace

RdmaListener::RdmaListener(std::uint16_t port) : fd_(make_listen_socket(port)) {}
RdmaListener::~RdmaListener() { if (fd_ >= 0) ::close(fd_); }

RdmaConnection::RdmaConnection() : impl_(new Impl) {}
RdmaConnection::~RdmaConnection() { delete impl_; }

void RdmaConnection::set_deadline(std::chrono::steady_clock::time_point deadline) {
    impl_->deadline = deadline;
}

bool RdmaConnection::rejected_handshake() const { return impl_->rejected_handshake; }

bool RdmaConnection::connect_client(const std::string& host, std::uint16_t tcp_port,
                                    std::uint64_t owner_token) {
    if (impl_->context || impl_->socket_fd >= 0) return false; // One-shot QP lifetime.
    const auto deadline = handshake_deadline(impl_->deadline);
    if (!create_resources(impl_, nullptr, 0, true)) return false;
    int fd = -1;
    // Only startup connection refusal is retried. One absolute deadline also
    // covers nonblocking connect and the complete QP/MR exchange.
    for (int attempt = 0; attempt < 100 && fd < 0 && Clock::now() < deadline; ++attempt) {
        fd = make_client_socket(host, tcp_port, deadline);
        if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (fd < 0) return false;
    impl_->socket_fd = fd;
    auto reject = [&] {
        impl_->failed.store(true, std::memory_order_release);
        ::shutdown(fd, SHUT_RDWR);
        return false;
    };
    WireInfo local{};
    if (!query_local(impl_, local)) return reject();
    local.owner_token = owner_token;
    if (!send_all(fd, &local, sizeof(local), deadline)) return reject();
    WireInfo remote{};
    if (!recv_all(fd, &remote, sizeof(remote), deadline) || Clock::now() >= deadline ||
        !transition_ready(impl_, remote)) return reject();
    impl_->peer_owner_token = remote.owner_token;
    remote_memory_ = {remote.address, remote.rkey, remote.length};
    connected_ = true;
    return true;
}

bool RdmaConnection::accept_server(std::uint16_t tcp_port, void* region, std::size_t region_length) {
    RdmaListener listener(tcp_port);
    return accept_server(listener, region, region_length);
}

bool RdmaConnection::accept_server(RdmaListener& listener, void* region, std::size_t region_length) {
    if (!listener.valid() || impl_->context || !create_resources(impl_, region, region_length, false)) return false;
    int fd = -1;
    do { fd = ::accept(listener.fd_, nullptr, nullptr); } while (fd < 0 && errno == EINTR);
    if (fd < 0) return false;
    impl_->socket_fd = fd;
    impl_->rejected_handshake = true;
    const auto deadline = handshake_deadline(impl_->deadline);
    enable_keepalive(fd);
    WireInfo remote{};
    if (!recv_all(fd, &remote, sizeof(remote), deadline)) return false;
    WireInfo local{};
    if (!query_local(impl_, local)) return false;
    local.address = reinterpret_cast<std::uint64_t>(impl_->region_mr->addr);
    local.rkey = impl_->region_mr->rkey;
    local.length = region_length;
    if (!transition_ready(impl_, remote) || !send_all(fd, &local, sizeof(local), deadline)) return false;
    impl_->peer_owner_token = remote.owner_token;
    remote_memory_ = {remote.address, remote.rkey, remote.length};
    impl_->rejected_handshake = false;
    connected_ = true;
    return true;
}

bool RdmaConnection::connected() const {
    return connected_ && !impl_->failed.load(std::memory_order_acquire);
}

RdmaOpTag RdmaConnection::peer_failure_tag() const {
    return impl_->peer_failure_tag;
}

std::uint64_t RdmaConnection::peer_owner_token() const {
    return impl_ ? impl_->peer_owner_token : 0;
}

std::uint64_t RdmaConnection::timeout_count() const {
    return impl_ ? impl_->rdma_timeouts.load(std::memory_order_relaxed) : 0;
}

std::uint64_t RdmaConnection::completion_error_count() const {
    return impl_ ? impl_->completion_errors.load(std::memory_order_relaxed) : 0;
}

std::uint64_t RdmaConnection::operation_count() const {
    return impl_ ? impl_->operations.load(std::memory_order_relaxed) : 0;
}

std::uint64_t RdmaConnection::timeout_count(RdmaOpTag tag) const {
    if (!impl_) return 0;
    const std::size_t index = std::min<std::size_t>(static_cast<std::size_t>(tag), 7);
    return impl_->timeouts_by_tag[index].load(std::memory_order_relaxed);
}

void RdmaConnection::set_operation_tag(RdmaOpTag tag) {
    if (impl_) impl_->op_tag = static_cast<std::uint8_t>(tag);
}

RdmaFailureInfo RdmaConnection::last_failure() const {
    if (!impl_) return RdmaFailureInfo{};
    std::lock_guard<std::mutex> guard(impl_->failure_mutex);
    return impl_->last_failure;
}

std::uint64_t RdmaConnection::failure_count() const {
    return impl_ ? impl_->failures.load(std::memory_order_relaxed) : 0;
}




bool RdmaConnection::read(std::uint64_t remote_address, std::uint32_t remote_rkey,
                          void* destination, std::size_t length) {
    if (!connected_ || impl_->failed) return false;
    if (!valid_transfer(destination, length, impl_->scratch.size())) return false;
    if (remote_address == 0) return false;
    ibv_sge sge{};
    sge.addr = reinterpret_cast<std::uintptr_t>(impl_->scratch.data());
    sge.length = static_cast<std::uint32_t>(length);
    sge.lkey = impl_->local_mr->lkey;
    ibv_send_wr wr{};
    wr.opcode = IBV_WR_RDMA_READ;
    wr.wr.rdma.remote_addr = remote_address;
    wr.wr.rdma.rkey = remote_rkey;
    if (!post_and_poll(impl_, wr, sge)) return false;
    std::memcpy(destination, impl_->scratch.data(), length);
    return true;
}

bool RdmaConnection::write(std::uint64_t remote_address, std::uint32_t remote_rkey,
                           const void* source, std::size_t length) {
    if (!connected_ || impl_->failed) return false;
    if (!valid_transfer(source, length, impl_->scratch.size())) return false;
    if (remote_address == 0) return false;
    std::memcpy(impl_->scratch.data(), source, length);
    ibv_sge sge{};
    sge.addr = reinterpret_cast<std::uintptr_t>(impl_->scratch.data());
    sge.length = static_cast<std::uint32_t>(length);
    sge.lkey = impl_->local_mr->lkey;
    ibv_send_wr wr{};
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.wr.rdma.remote_addr = remote_address;
    wr.wr.rdma.rkey = remote_rkey;
    return post_and_poll(impl_, wr, sge);
}

bool RdmaConnection::compare_swap(std::uint64_t remote_address, std::uint32_t remote_rkey,
                                  std::uint64_t compare, std::uint64_t swap,
                                  std::uint64_t& previous) {
    if (!connected_ || impl_->failed) return false;
    if (!valid_atomic_target(remote_address)) return false;
    std::uint64_t result = 0;
    ibv_sge sge{};
    sge.addr = reinterpret_cast<std::uintptr_t>(&result);
    sge.length = sizeof(result);
    sge.lkey = impl_->local_mr->lkey;
    // The result buffer must be registered; use the beginning of scratch.
    std::memcpy(impl_->scratch.data(), &result, sizeof(result));
    sge.addr = reinterpret_cast<std::uintptr_t>(impl_->scratch.data());
    ibv_send_wr wr{};
    wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
    wr.wr.atomic.remote_addr = remote_address;
    wr.wr.atomic.rkey = remote_rkey;
    wr.wr.atomic.compare_add = compare;
    wr.wr.atomic.swap = swap;
    if (!post_and_poll(impl_, wr, sge)) return false;
    std::memcpy(&previous, impl_->scratch.data(), sizeof(previous));
    return true;
}


bool RdmaConnection::read_batch(const std::vector<RdmaReadRequest>& requests) {
    if (requests.empty()) return true;
    if (!connected_ || impl_->failed || requests.size() > 128) return false;
    std::size_t total = 0;
    for (const auto& request : requests) {
        if (request.remote_address == 0 ||
            !valid_transfer(request.destination, request.length, impl_->scratch.size()) ||
            total > impl_->scratch.size() - request.length)
            return false;
        total += request.length;
    }
    if (requests.size() == 1) {
        const auto& request = requests.front();
        return read(request.remote_address, request.remote_rkey,
                    request.destination, request.length);
    }
    std::vector<ibv_send_wr> wrs(requests.size());
    std::vector<ibv_sge> sges(requests.size());
    std::vector<std::size_t> offsets(requests.size());
    std::size_t offset = 0;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        offsets[i] = offset;
        sges[i].addr = reinterpret_cast<std::uintptr_t>(impl_->scratch.data() + offset);
        sges[i].length = static_cast<std::uint32_t>(request.length);
        sges[i].lkey = impl_->local_mr->lkey;
        wrs[i].opcode = IBV_WR_RDMA_READ;
        wrs[i].wr.rdma.remote_addr = request.remote_address;
        wrs[i].wr.rdma.rkey = request.remote_rkey;
        offset += request.length;
    }
    if (!post_batch_and_poll(impl_, wrs, sges)) return false;
    for (std::size_t i = 0; i < requests.size(); ++i)
        std::memcpy(requests[i].destination, impl_->scratch.data() + offsets[i], requests[i].length);
    return true;
}

bool RdmaConnection::write_batch(const std::vector<RdmaWriteRequest>& requests) {
    if (requests.empty()) return true;
    if (!connected_ || impl_->failed || requests.size() > 128) return false;
    std::size_t total = 0;
    for (const auto& request : requests) {
        if (request.remote_address == 0 ||
            !valid_transfer(request.source, request.length, impl_->scratch.size()) ||
            total > impl_->scratch.size() - request.length)
            return false;
        total += request.length;
    }
    if (requests.size() == 1) {
        const auto& request = requests.front();
        return write(request.remote_address, request.remote_rkey,
                     request.source, request.length);
    }
    std::vector<ibv_send_wr> wrs(requests.size());
    std::vector<ibv_sge> sges(requests.size());
    std::size_t offset = 0;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        std::memcpy(impl_->scratch.data() + offset, request.source, request.length);
        sges[i].addr = reinterpret_cast<std::uintptr_t>(impl_->scratch.data() + offset);
        sges[i].length = static_cast<std::uint32_t>(request.length);
        sges[i].lkey = impl_->local_mr->lkey;
        wrs[i].opcode = IBV_WR_RDMA_WRITE;
        wrs[i].wr.rdma.remote_addr = request.remote_address;
        wrs[i].wr.rdma.rkey = request.remote_rkey;
        offset += request.length;
    }
    return post_batch_and_poll(impl_, wrs, sges);
}

bool RdmaConnection::compare_swap_batch(const std::vector<RdmaCompareSwapRequest>& requests) {
    if (requests.empty()) return true;
    if (!connected_ || impl_->failed || requests.size() > 128) return false;
    const std::size_t bytes = requests.size() * sizeof(std::uint64_t);
    if (bytes > impl_->scratch.size()) return false;
    for (const auto& request : requests)
        if (!request.previous || !valid_atomic_target(request.remote_address)) return false;
    if (requests.size() == 1) {
        const auto& request = requests.front();
        return compare_swap(request.remote_address, request.remote_rkey,
                            request.compare, request.swap, *request.previous);
    }
    std::vector<ibv_send_wr> wrs(requests.size());
    std::vector<ibv_sge> sges(requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i) {
        sges[i].addr = reinterpret_cast<std::uintptr_t>(impl_->scratch.data() + i * sizeof(std::uint64_t));
        sges[i].length = sizeof(std::uint64_t);
        sges[i].lkey = impl_->local_mr->lkey;
        wrs[i].opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
        wrs[i].wr.atomic.remote_addr = requests[i].remote_address;
        wrs[i].wr.atomic.rkey = requests[i].remote_rkey;
        wrs[i].wr.atomic.compare_add = requests[i].compare;
        wrs[i].wr.atomic.swap = requests[i].swap;
    }
    if (!post_batch_and_poll(impl_, wrs, sges)) return false;
    for (std::size_t i = 0; i < requests.size(); ++i)
        std::memcpy(requests[i].previous, impl_->scratch.data() + i * sizeof(std::uint64_t), sizeof(std::uint64_t));
    return true;
}

bool RdmaConnection::wait_for_disconnect() {
    if (impl_->socket_fd < 0) return false;
    std::uint8_t byte = 0;
    ssize_t received = 0;
    do {
        received = ::recv(impl_->socket_fd, &byte, 1, 0);
    } while (received < 0 && errno == EINTR);
    if (received == 1 && (byte & 0x80u) && (byte & 0x7fu) <= 5)
        impl_->peer_failure_tag = static_cast<RdmaOpTag>(byte & 0x7fu);
    return received >= 0;
}

}  // namespace rrl
