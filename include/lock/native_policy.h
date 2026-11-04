#pragma once
#include "lock/wait_policy.h"

namespace rrl {
// Registered passive state, corresponding to Deneva's RdmaTxnTableNode.
// Only the NIC accesses this memory after registration.
class NativeTxnMemory {
public:
    explicit NativeTxnMemory(std::size_t slots = 4096);
    ~NativeTxnMemory();
    void* data();
    std::size_t bytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Physical range slots are the sole lock-grant authority. No transaction RPC.
class NativePolicyClient {
public:
    NativePolicyClient(const std::string& host, std::uint16_t port,
                       IRangeLockManager& physical);
    ~NativePolicyClient();
    PolicyReply begin(WaitPolicy, std::uint64_t age, std::uint64_t budget_ns);
    PolicyReply acquire(txnid_t, LockMode, const KeyRangeEnt&, LockKind);
    PolicyReply check(txnid_t);
    PolicyReply prepare_commit(txnid_t);
    // Caller MUST complete data writeback and physical unlock_all before end.
    PolicyReply end(txnid_t);
    std::uint64_t rdma_operations() const;
    RC last_lock_result() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
int run_native_txn_server(std::uint16_t port, std::size_t slots = 4096);
} // namespace rrl
