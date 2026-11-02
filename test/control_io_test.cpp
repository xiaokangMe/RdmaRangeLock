#include "rdma/control_io.h"
#include <cstdlib>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
using namespace rrl;
namespace {
void check(bool ok, const char* what) { if (!ok) { std::cerr << "FAIL: " << what << "\n"; std::exit(1); } }
}
int main() {
    int pair[2]{};
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    std::uint64_t sent = 0x1234, received = 0;
    auto deadline = control::Clock::now() + std::chrono::seconds(1);
    check(control::send_all(pair[0], &sent, sizeof(sent), deadline) &&
          control::recv_all(pair[1], &received, sizeof(received), deadline) && sent == received, "complete exchange");
    check(::send(pair[0], &sent, 1, 0) == 1, "partial handshake");
    const auto began = control::Clock::now();
    deadline = began + std::chrono::milliseconds(100);
    check(!control::recv_all(pair[1], &received, sizeof(received), deadline), "partial handshake expires");
    check(control::Clock::now() >= deadline && control::Clock::now() - began < std::chrono::seconds(2), "absolute receive deadline");
    std::vector<std::uint8_t> bytes(8u << 20);
    deadline = control::Clock::now() + std::chrono::milliseconds(100);
    check(!control::send_all(pair[0], bytes.data(), bytes.size(), deadline), "blocked send expires");
    ::close(pair[0]); ::close(pair[1]);
    std::cout << "control I/O deadlines: OK\n";
}
