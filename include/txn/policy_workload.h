#pragma once
#include "lock/wait_policy.h"
#include "common/latency_histogram.h"
#include <functional>
namespace rrl {
enum class PolicyProtocol { Record, Gap, NextKey, Hierarchy, Predicate, None, ExactRange, PriorKey, Precision, Griffin };
struct PolicyRunConfig {
    WaitPolicy wait_policy{WaitPolicy::NoWait};
    PolicyProtocol protocol{PolicyProtocol::Hierarchy};
    std::uint64_t max_attempts{1000}, wait_timeout_ms{5000};
    std::size_t lock_budget{16384};
    std::size_t rdma_lock_slots_per_shard{0}; // 0 accepts server-advertised geometry
    bool leaf_hierarchy{false},dag_hierarchy{false},separator_hierarchy{false};
    std::size_t hierarchy_index_paths{2},hierarchy_read_path{0};
    // Zero keeps static preplanning. Conversion releases only redundant
    // physical representation after equivalent logical protection exists.
    std::size_t hierarchy_escalation_threshold{0},hierarchy_deescalate_after{0};
    // Deneva-style per-record object metadata. This is an explicit native
    // path for rdma-record-2pl; range protocols keep the range-table fallback.
    bool native_object_locks{false};
    // Explicit source compatibility; default strict keeps transactional locks.
    bool deneva_insert_behavior{false};
    std::string coordinator_host;
    std::string histogram_path;
    std::uint16_t coordinator_port{0};
    // Deneva-style passive transaction table; zero keeps the compatibility path.
    std::uint16_t txn_state_port{0};
    // False runs work directly on OS threads, without scheduler/stacks.
    bool use_coroutines{true};
    std::size_t coroutines{1}, coroutine_stack_bytes{262144}, warmup_seconds{0};
    // Called after all warm-up attempts drain, before measured workers resume.
    std::function<void()> measurement_begin;
};
struct PolicyStats {
    bool enabled{false};
    std::uint64_t escalation_attempts{0},escalations{0},escalation_failures{0};
    std::uint64_t deescalation_attempts{0},deescalations{0},deescalation_failures{0};
    std::uint64_t warmup_committed{0},warmup_aborted{0},warmup_elapsed_ns{0};
    std::uint64_t coroutine_switches{0},state_rdma_operations{0};
    std::uint64_t deneva_early_inserts{0},deneva_early_unlocks{0},deneva_split_aborts{0},deneva_partial_write_aborts{0};
    std::uint64_t index_pending_aborts{0};
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
