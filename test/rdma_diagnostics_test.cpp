#include "lock/remote_lock_manager.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
using namespace rrl;
namespace rrl {
// Test-only access: no runtime failure injection or new lock-manager method.
struct RemoteLockManagerTestPeer {
    static RdmaConnection& connection(RemoteLockManager& manager, std::size_t index) {
        return *manager.connections_[index]->connection;
    }
};
}
namespace {
void check(bool ok, const char* what) { if (!ok) { std::cerr << "FAIL: " << what << "\n"; std::exit(1); } }
}
int main(int argc, char**) {
    const char* host = std::getenv("RRL_TEST_RDMA_HOST");
    const char* text = std::getenv("RRL_TEST_RDMA_PORT");
    if (!host || !text) return 77;
    const auto port = static_cast<std::uint16_t>(std::stoul(text));
    if (argc > 1) {
        RdmaConnection rejected;
        const auto began = std::chrono::steady_clock::now();
        check(!rejected.connect_client(host, port), "incomplete peer exchange rejected");
        check(std::chrono::steady_clock::now() - began < std::chrono::seconds(2), "client handshake is bounded");
        check(!rejected.connected() && rejected.operation_count() == 0, "no data operations after failed handshake");
        return 0;
    }
    RemoteLockManager manager;
    manager.set_connection_count(2);
    check(manager.connect(host, port), "manager connect");
    const auto before = manager.diagnostics();
    check(manager.get_lock_count() == 0 && manager.diagnostics().count_errors == before.count_errors,
          "healthy empty table does not increment count errors");
    std::uint64_t values[3]{};
    std::uint64_t last_sequence = 0;
    for (std::size_t i = 0; i < 2; ++i) {
        auto& connection = RemoteLockManagerTestPeer::connection(manager, i);
        const auto memory = connection.remote_memory();
        connection.set_operation_tag(RdmaOpTag::LoadCounts);
        check(!connection.read_batch({
            {memory.address, memory.rkey, &values[0], 8},
            {memory.address + 8, memory.rkey ^ 0x80000000u, &values[1], 8},
            {memory.address + 16, memory.rkey, &values[2], 8}}), "real middle-WR completion error");
        const auto snapshot = connection.last_failure();
        check(snapshot.wr_index == 1 && snapshot.remote_address == memory.address + 8 &&
              snapshot.length == 8 && snapshot.opcode == IBV_WR_RDMA_READ &&
              snapshot.status == IBV_WC_REM_ACCESS_ERR && snapshot.tag == static_cast<unsigned>(RdmaOpTag::LoadCounts),
              "failure identifies actual middle WR");
        check(snapshot.sequence > last_sequence && manager.last_failure().sequence == snapshot.sequence &&
              manager.last_failure().qp_num == snapshot.qp_num, "latest failure chosen across QPs");
        last_sequence = snapshot.sequence;
    }
    check(manager.get_lock_count() == 0 && manager.diagnostics().count_errors == before.count_errors + 1 &&
          manager.diagnostics().last_count_error == ERROR, "count failure distinguishable from healthy zero");
    RdmaConnection expired;
    check(expired.connect_client(host, port), "deadline probe connect");
    const auto memory = expired.remote_memory();
    expired.set_deadline(std::chrono::steady_clock::now() - std::chrono::seconds(1));
    check(!expired.read_batch({{memory.address, memory.rkey, &values[0], 8},
                               {memory.address + 8, memory.rkey, &values[1], 8}}), "expired batch rejected before post");
    const auto snapshot = expired.last_failure();
    check(snapshot.wr_index == kRdmaUnknownWr && snapshot.remote_address == 0 && snapshot.length == 0 &&
          expired.timeout_count() == 1 && expired.operation_count() == 0 && snapshot.sequence > last_sequence,
          "unknown timeout does not blame first WR");
    std::cout << "RDMA diagnostics: OK\n";
}
