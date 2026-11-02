#pragma once
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <poll.h>
#include <sys/socket.h>
namespace rrl { namespace control {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
inline bool wait_fd(int fd, short events, Deadline deadline) {
    for (;;) {
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) return false;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
        pollfd event{fd, events, 0};
        const int ready = ::poll(&event, 1, static_cast<int>(std::min<std::int64_t>(INT_MAX, ms + 1)));
        if (ready < 0 && errno == EINTR) continue;
        return ready > 0 && !(event.revents & POLLNVAL);
    }
}
inline bool send_all(int fd, const void* data, std::size_t length, Deadline deadline) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (length) {
        if (!wait_fd(fd, POLLOUT, deadline)) return false;
        const auto sent = ::send(fd, bytes, length, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (sent <= 0) return false;
        bytes += sent; length -= static_cast<std::size_t>(sent);
    }
    return true;
}
inline bool recv_all(int fd, void* data, std::size_t length, Deadline deadline) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    while (length) {
        if (!wait_fd(fd, POLLIN, deadline)) return false;
        const auto received = ::recv(fd, bytes, length, MSG_DONTWAIT);
        if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (received <= 0) return false;
        bytes += received; length -= static_cast<std::size_t>(received);
    }
    return true;
}
} } // namespace rrl::control
