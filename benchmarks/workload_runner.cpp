#include "txn/workload.h"
#include "lock/remote_lock_manager.h"

#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <regex>
#include <string>

using namespace rrl;


namespace {

std::string config_value(const std::string& text, const std::string& key) {
    const std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*(?:\\\"([^\\\"]*)\\\"|([-+0-9.eE]+))");
    std::smatch match;
    if (!std::regex_search(text, match, pattern)) return {};
    return match[1].matched ? match[1].str() : match[2].str();
}

std::string first_config_value(const std::string& text,
                               std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const auto value = config_value(text, key);
        if (!value.empty()) return value;
    }
    return {};
}

bool load_config(const std::string& path, WorkloadConfig& config,
                 std::string& index_name, std::string& cc) {
    std::ifstream input(path);
    if (!input) return false;
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    auto assign_u64 = [&](std::initializer_list<const char*> keys, std::size_t& target) {
        const auto value = first_config_value(text, keys);
        if (!value.empty()) target = std::stoull(value);
    };
    auto assign_double = [&](std::initializer_list<const char*> keys, double& target) {
        const auto value = first_config_value(text, keys);
        if (!value.empty()) target = std::stod(value);
    };
    auto assign_string = [&](std::initializer_list<const char*> keys, std::string& target) {
        const auto value = first_config_value(text, keys);
        if (!value.empty()) target = value;
    };
    std::string workload;
    assign_string({"workload"}, workload);
    if (!workload.empty()) config.kind = WorkloadTrace::parse_kind(workload);
    assign_string({"index"}, index_name);
    assign_string({"cc"}, cc);
    assign_u64({"key_count", "keys"}, config.key_count);
    assign_u64({"transaction_count", "transactions"}, config.transaction_count);
    assign_u64({"operations_per_transaction", "operations"}, config.operations_per_transaction);
    assign_u64({"range_length", "range"}, config.range_length);
    assign_u64({"threads"}, config.threads);
    assign_u64({"hierarchy_bucket_width", "bucket"}, config.hierarchy_bucket_width);
    assign_u64({"tpcc_items_per_order", "items"}, config.tpcc_items_per_order);
    assign_u64({"seed"}, config.seed);
    assign_double({"zipf_theta", "zipf"}, config.zipf_theta);
    if (index_name == "btree_latch_coupling") index_name = "v2";
    if (index_name == "btree_single") index_name = "v1";
    if (cc == "record-level-strict-2pl") cc = "record-2pl";
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    WorkloadConfig config;
    std::string index_name = "v2";
    std::string cc = "none";
    std::string rdma_host = "127.0.0.1";
    std::uint16_t rdma_port = 19876;
    bool csv = false;
    std::string config_path;
    for (int i = 1; i + 1 < argc; i += 2)
        if (std::string(argv[i]) == "--config") config_path = argv[i + 1];
    if (!config_path.empty() && !load_config(config_path, config, index_name, cc)) {
        std::cerr << "failed to read config: " << config_path << "\n";
        return 2;
    }
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        const std::string value = argv[i + 1];
        if (option == "--config") continue;
        if (option == "--workload") config.kind = WorkloadTrace::parse_kind(value);
        else if (option == "--index") index_name = value;
        else if (option == "--cc") cc = value;
        else if (option == "--keys") config.key_count = std::stoull(value);
        else if (option == "--txns") config.transaction_count = std::stoull(value);
        else if (option == "--ops") config.operations_per_transaction = std::stoull(value);
        else if (option == "--range") config.range_length = std::stoull(value);
        else if (option == "--bucket") config.hierarchy_bucket_width = std::stoull(value);
        else if (option == "--rdma-host") rdma_host = value;
        else if (option == "--rdma-port") rdma_port = static_cast<std::uint16_t>(std::stoull(value));
        else if (option == "--threads") config.threads = std::stoull(value);
        else if (option == "--seed") config.seed = std::stoull(value);
        else if (option == "--zipf") config.zipf_theta = std::stod(value);
        else if (option == "--items") config.tpcc_items_per_order = std::stoull(value);
        else if (option == "--format" && value == "csv") csv = true;
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
        LockManager locks;
        locks.set_bucket_width(config.hierarchy_bucket_width);
        stats = run_hierarchical_lock(*index, locks, trace, config.threads);
    } else if (cc == "rdma-next-key" || cc == "rdma-hierarchical") {
        RemoteLockManager locks;
        locks.set_connection_count(config.threads);
        if (!locks.connect(rdma_host, rdma_port)) {
            std::cerr << "failed to connect RDMA lock server at " << rdma_host << ":" << rdma_port << "\n";
            return 2;
        }
        locks.set_bucket_width(config.hierarchy_bucket_width);
        if (cc == "rdma-next-key") stats = run_next_key_lock(*index, locks, trace, config.threads);
        else stats = run_hierarchical_lock(*index, locks, trace, config.threads);
    } else {
        stats = run_workload(*index, trace, config.threads);
    }

    if (csv) {
        std::cout << "workload,index,cc,threads,transactions,committed,aborted,operations,reads,updates,inserts,deletes,range_reads,rows_found,rows_scanned,rows_rechecked,lock_acquires,lock_conflicts,lock_capacity_aborts,lock_gate_aborts,throughput_committed_txn_s,throughput_attempt_txn_s,checksum\n";
        std::cout << WorkloadTrace::name(config.kind) << ',' << index_name << ',' << cc << ',' << config.threads << ','
                  << stats.transactions << ',' << stats.committed << ',' << stats.aborted << ',' << stats.operations << ','
                  << stats.reads << ',' << stats.updates << ',' << stats.inserts << ',' << stats.deletes << ','
                  << stats.range_reads << ',' << stats.rows_found << ',' << stats.rows_scanned << ',' << stats.rows_rechecked << ','
                  << stats.lock_acquires << ',' << stats.lock_conflicts << ',' << stats.lock_capacity_aborts << ',' << stats.lock_gate_aborts << ',' << stats.throughput() << ','
                  << stats.attempt_throughput() << ',' << stats.checksum << '\n';
    } else {
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
                  << ",rows_rechecked=" << stats.rows_rechecked
                  << ",lock_acquires=" << stats.lock_acquires
                  << ",lock_conflicts=" << stats.lock_conflicts
                  << ",lock_capacity_aborts=" << stats.lock_capacity_aborts
                  << ",lock_gate_aborts=" << stats.lock_gate_aborts
                  << ",throughput_committed_txn_s=" << stats.throughput()
                  << ",throughput_attempt_txn_s=" << stats.attempt_throughput()
                  << ",checksum=" << stats.checksum << "\n";
    }
    return 0;
}
