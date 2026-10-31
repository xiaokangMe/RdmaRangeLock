#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/types.h"
#include "index/btree.h"
#include "lock/lock_manager.h"

namespace rrl {

enum class WorkloadKind : std::uint8_t { YcsbA, YcsbB, YcsbE, TpccPayment, TpccNewOrder };
enum class OperationKind : std::uint8_t { Read, Update, Insert, Delete, RangeRead };

struct WorkloadConfig {
    WorkloadKind kind{WorkloadKind::YcsbA};
    size_t key_count{10000};
    size_t transaction_count{10000};
    size_t operations_per_transaction{10};
    size_t range_length{10};
    size_t threads{1};
    size_t hierarchy_bucket_width{64};
    size_t tpcc_items_per_order{5};
    double zipf_theta{0.99};
    uint64_t seed{7};
};

struct Operation { OperationKind kind{OperationKind::Read}; idx_key_t key{0}; idx_key_t end_key{0}; uint64_t value{0}; };
struct TransactionTrace { txnid_t txn_id{0}; std::vector<Operation> operations; };

class WorkloadTrace {
public:
    static WorkloadTrace generate(const WorkloadConfig&);
    const std::vector<TransactionTrace>& transactions() const { return transactions_; }
    size_t max_key() const { return max_key_; }
    const WorkloadConfig& config() const { return config_; }
    static WorkloadKind parse_kind(const std::string&);
    static const char* name(WorkloadKind);
private:
    WorkloadConfig config_{};
    std::vector<TransactionTrace> transactions_;
    size_t max_key_{0};
};

struct RunStats {
    uint64_t transactions{0}, committed{0}, aborted{0}, operations{0};
    uint64_t reads{0}, updates{0}, inserts{0}, deletes{0};
    uint64_t range_reads{0}, rows_found{0}, rows_scanned{0}, rows_rechecked{0};
    uint64_t lock_acquires{0}, lock_conflicts{0}, lock_capacity_aborts{0}, lock_gate_aborts{0};
    uint64_t checksum{0}, elapsed_ns{0};
    double throughput() const;
    double attempt_throughput() const;
};

RunStats run_workload(BTreeIndex&, const WorkloadTrace&, size_t threads = 1);
RunStats run_record_2pl(BTreeIndex&, LockManager&, const WorkloadTrace&, size_t threads = 1);
RunStats run_gap_lock(BTreeIndex&, LockManager&, const WorkloadTrace&, size_t threads = 1);
RunStats run_next_key_lock(BTreeIndex&, IRangeLockManager&, const WorkloadTrace&, size_t threads = 1);
RunStats run_hierarchical_lock(BTreeIndex&, IRangeLockManager&, const WorkloadTrace&, size_t threads = 1);

}  // namespace rrl
