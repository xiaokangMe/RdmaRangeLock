#include "lock/remote_lock_manager.h"
#include <cstdlib>
#include <iostream>
using namespace rrl;
namespace {
void check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; std::exit(1); }
}
}
int main() {
    const char* host = std::getenv("RRL_TEST_RDMA_HOST");
    const char* text = std::getenv("RRL_TEST_RDMA_PORT");
    if (!host || !text) return 77;
    setenv("RRL_RDMA_PROFILE", "1", 1);
    RemoteLockManager manager;
    check(manager.connect(host, static_cast<std::uint16_t>(std::stoul(text))), "connect");
    const auto point = KeyRangeEnt::point(900001);
    check(manager.lock(1, LockMode::S, point) == RCOK, "new lock");
    check(manager.lock(1, LockMode::S, point) == RCOK, "cache hit");
    check(manager.lock(2, LockMode::X, point) == AbortConflict, "conflict");
    check(manager.unlock_all(1) == RCOK, "release");
    const auto p = manager.diagnostics();
    check(p.profile_enabled && p.lock_calls == 3 && p.cache_hits == 1, "logical calls and cache");
    check(p.sync_calls == 13 && manager.rdma_operations() == 14, "synchronous calls differ from WR count");
    check(p.phase_calls == std::array<std::uint64_t, 6>{0, 3, 3, 2, 2, 3}, "phase calls");
    check(p.read_bytes == 152 && p.write_bytes == 80, "submitted data bytes exclude CAS");
    std::uint64_t sum = 0;
    for (auto ns : p.phase_ns) sum += ns;
    check(sum == p.sync_ns && p.sync_ns > 0 && p.gate_hold_ns >= p.sync_ns / 2,
          "phase durations aggregate");
    check(p.local_errors == 0 && p.slot_checks == 1, "healthy profile");
    setenv("RRL_RDMA_PROFILE", "0", 1);
    RemoteLockManager disabled;
    check(disabled.connect(host, static_cast<std::uint16_t>(std::stoul(text))), "profile off connect");
    check(disabled.lock(3, LockMode::S, point) == RCOK && disabled.unlock_all(3) == RCOK, "profile off workload");
    const auto off = disabled.diagnostics();
    check(!off.profile_enabled && off.sync_calls == 0 && off.sync_ns == 0 &&
          off.lock_calls == 0 && off.io_wait_ns == 0 && off.gate_hold_ns == 0 &&
          off.read_bytes == 0 && off.write_bytes == 0 && off.lookup_ns == 0 &&
          disabled.rdma_operations() > 0, "profile off keeps counters disabled");
    std::cout << "RDMA profile: OK\n";
}
