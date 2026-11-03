#pragma once
#include "lock/wait_policy.h"
#include "common/latency_histogram.h"
namespace rrl {
enum class PolicyProtocol { Record, Gap, NextKey, Hierarchy, Predicate };
struct PolicyRunConfig {
    WaitPolicy wait_policy{WaitPolicy::NoWait};
    PolicyProtocol protocol{PolicyProtocol::Hierarchy};
    std::uint64_t max_attempts{1000}, wait_timeout_ms{5000};
    std::size_t lock_budget{16384};
    std::string coordinator_host;
    std::string histogram_path;
    std::uint16_t coordinator_port{0};
};
struct PolicyStats {
    bool enabled{false};
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
