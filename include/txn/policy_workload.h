#pragma once
#include "lock/wait_policy.h"
#include "common/latency_histogram.h"
#include <functional>
namespace rrl {
enum class PolicyProtocol { Record, Gap, NextKey, Hierarchy, Predicate, None };
struct PolicyRunConfig {
    WaitPolicy wait_policy{WaitPolicy::NoWait};
    PolicyProtocol protocol{PolicyProtocol::Hierarchy};
    std::uint64_t max_attempts{1000}, wait_timeout_ms{5000};
    std::size_t lock_budget{16384};
    bool leaf_hierarchy{false};
    // Explicit source compatibility; default strict keeps transactional locks.
    bool deneva_insert_behavior{false};
    std::string coordinator_host;
    std::string histogram_path;
    std::uint16_t coordinator_port{0};
    // Deneva-style passive transaction table; zero keeps the compatibility path.
    std::uint16_t txn_state_port{0};
    std::size_t coroutines{1}, coroutine_stack_bytes{262144}, warmup_seconds{0};
    // Called after all warm-up attempts drain, before measured workers resume.
    std::function<void()> measurement_begin;
};
struct PolicyStats {
    bool enabled{false};
    std::uint64_t warmup_committed{0},warmup_aborted{0},warmup_elapsed_ns{0};
    std::uint64_t coroutine_switches{0},state_rdma_operations{0};
    std::uint64_t deneva_early_inserts{0},deneva_early_unlocks{0},deneva_split_aborts{0},deneva_partial_write_aborts{0};
    std::uint64_t logical_started{0},unfinished{0},logical_failed{0},retries{0},max_retries{0};
    std::uint64_t completed_retries{0},unfinished_retries{0};
    std::uint64_t wound_aborts{0},deadline_aborts{0},retry_exhausted{0};
    LatencySummary successful_attempt,logical_completion,lock_wait,lock_call,release;
};
class BTreeIndex;
class WorkloadTrace;
struct RunStats;
RunStats run_policy_workload(BTreeIndex&,IRangeLockManager&,const WorkloadTrace&,const PolicyRunConfig&);
} // namespace rrl
