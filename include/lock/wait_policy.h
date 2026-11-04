#pragma once
#include "lock/lock_manager.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace rrl {
enum class WaitPolicy : std::uint64_t { NoWait, WaitDie, WoundWait };
WaitPolicy parse_wait_policy(const std::string&);
const char* wait_policy_name(WaitPolicy);
enum class TxnResult : std::uint64_t { Ok, Conflict, Wounded, Deadline, Error };
struct PolicyReply {
    TxnResult result{TxnResult::Error};
    txnid_t id{0};
    std::uint64_t wait_ns{0};
    std::uint64_t age{0};
};
// A lifecycle interface separate from the unchanged IRangeLockManager API.
// PrepareCommit is the serialization point between a wound and commit.
class TransactionCoordinator {
public:
    explicit TransactionCoordinator(std::size_t lock_budget = 16384, bool exact_objects = false);
    ~TransactionCoordinator();
    PolicyReply begin(std::uint64_t session, WaitPolicy policy, std::uint64_t age,
                      std::uint64_t budget_ns);
    PolicyReply acquire(std::uint64_t session, txnid_t, LockMode, const KeyRangeEnt&, LockKind);
    PolicyReply check(std::uint64_t session, txnid_t);
    PolicyReply prepare_commit(std::uint64_t session, txnid_t);
    PolicyReply end(std::uint64_t session, txnid_t);
    void disconnect(std::uint64_t session);
    void shutdown();
    std::size_t active() const;
    std::size_t waiting() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One client handle per worker. Local and distributed runs use the same engine.
// A remote coordinator is a TCP control plane; physical slots still use RDMA.
class PolicyClient {
public:
    explicit PolicyClient(TransactionCoordinator& local, std::uint64_t session);
    PolicyClient(const std::string& host, std::uint16_t port, std::uint64_t timeout_ms);
    ~PolicyClient();
    PolicyReply begin(WaitPolicy, std::uint64_t age, std::uint64_t budget_ns);
    PolicyReply acquire(txnid_t, LockMode, const KeyRangeEnt&, LockKind);
    PolicyReply check(txnid_t);
    PolicyReply prepare_commit(txnid_t);
    PolicyReply end(txnid_t);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Runs until the process is terminated. Failures poison the coordinator and
// wake all waiters; metadata is never cleared ahead of physical RDMA recovery.
int run_wait_coordinator(std::uint16_t port,std::size_t lock_budget=16384,std::uint64_t idle_timeout_ms=10000);
} // namespace rrl
