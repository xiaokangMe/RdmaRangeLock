#include "txn/workload.h"
#include <iostream>
#include <string>

using namespace rrl;

int main(int argc, char** argv) {
    WorkloadConfig config;
    std::string index_name = "v2";
    std::string cc = "none";
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        const std::string value = argv[i + 1];
        if (option == "--workload") config.kind = WorkloadTrace::parse_kind(value);
        else if (option == "--index") index_name = value;
        else if (option == "--cc") cc = value;
        else if (option == "--keys") config.key_count = std::stoull(value);
        else if (option == "--txns") config.transaction_count = std::stoull(value);
        else if (option == "--ops") config.operations_per_transaction = std::stoull(value);
        else if (option == "--range") config.range_length = std::stoull(value);
        else if (option == "--bucket") config.hierarchy_bucket_width = std::stoull(value);
        else if (option == "--threads") config.threads = std::stoull(value);
        else if (option == "--seed") config.seed = std::stoull(value);
        else if (option == "--zipf") config.zipf_theta = std::stod(value);
        else if (option == "--items") config.tpcc_items_per_order = std::stoull(value);
    }

    const auto trace = WorkloadTrace::generate(config);
    BTreeSingle v1;
    BTreeLatch v2;
    BTreeIndex* index = nullptr;
    if (index_name == "v1") {
        v1.init(128); index = &v1;
        if (cc == "none") config.threads = 1;
    } else {
        v2.init(128); index = &v2;
    }

    RunStats stats;
    if (cc == "record-2pl") {
        LockManager locks; stats = run_record_2pl(*index, locks, trace, config.threads);
    } else if (cc == "gap-lock") {
        LockManager locks; stats = run_gap_lock(*index, locks, trace, config.threads);
    } else if (cc == "next-key") {
        LockManager locks; stats = run_next_key_lock(*index, locks, trace, config.threads);
    } else if (cc == "hierarchical") {
        LockManager locks; stats = run_hierarchical_lock(*index, locks, trace, config.threads);
    } else {
        stats = run_workload(*index, trace, config.threads);
    }

    std::cout << "workload=" << WorkloadTrace::name(config.kind)
              << ",index=" << index_name << ",cc=" << cc
              << ",threads=" << config.threads
              << ",transactions=" << stats.transactions
              << ",committed=" << stats.committed
              << ",aborted=" << stats.aborted
              << ",operations=" << stats.operations
              << ",reads=" << stats.reads
              << ",updates=" << stats.updates
              << ",inserts=" << stats.inserts
              << ",range_reads=" << stats.range_reads
              << ",rows_found=" << stats.rows_found
              << ",rows_scanned=" << stats.rows_scanned
              << ",lock_acquires=" << stats.lock_acquires
              << ",lock_conflicts=" << stats.lock_conflicts
              << ",throughput_txn_s=" << stats.throughput()
              << ",checksum=" << stats.checksum << "\n";
    return 0;
}