#include "txn/workload.h"
#include "lock/remote_lock_manager.h"
#include "common/config_parse.h"

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <limits>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <set>
#include <stdexcept>
#include <arpa/inet.h>
#include <string>

using namespace rrl;

namespace {

// Bucket width policy for v6, measured rather than guessed: see
// docs/notes/v6-bucket-width-2026-11-01.md. A range scan wants a wide bucket so
// one lock covers many rows; every point-write workload measured so far wants a
// narrow bucket so unrelated keys do not share a lock. The TPC-C workloads were
// swept as well and follow the point-write case, not the middle value.
std::size_t auto_bucket_width(WorkloadKind kind) {
    switch (kind) {
        case WorkloadKind::YcsbE: return 256;
        case WorkloadKind::YcsbA:
        case WorkloadKind::YcsbB:
        case WorkloadKind::TpccPayment:
        case WorkloadKind::TpccNewOrder:
        default: return 16;
    }
}

// RDMA connection policy, measured rather than assumed. A connection owns one
// QP, one completion queue and one scratch buffer, so it can serve exactly one
// thread at a time; extra connections only add more callers racing for the same
// 16 remote gates. On the loopback pair (2,000-3,000 transactions per point,
// docs/notes/rdma-connection-policy-2026-11-01.md) two connections beat
// one-per-thread by 2.17x at t=8 and 2.56x at t=16, and were within 10% of the
// best point at t=4. The published matrix therefore uses min(threads, 2) unless
// --rdma-connections overrides it.
constexpr std::size_t kDefaultRdmaConnections = 2;

std::size_t resolve_connection_count(std::size_t threads, std::size_t requested) {
    if (requested != 0) return std::max<std::size_t>(1, requested);
    return std::max<std::size_t>(1, std::min<std::size_t>(threads, kDefaultRdmaConnections));
}

void load_config(const std::string& path, WorkloadConfig& config,
                 std::string& index_name, std::string& cc, std::size_t& order,
                 std::string& host, std::uint16_t& port, std::size_t& connections,
                 bool& bucket_auto) {
    std::ifstream input(path);
    if (!input) throw std::invalid_argument("failed to read config: " + path);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    const auto values = FlatConfigParser(text).parse();
    const std::set<std::string> metadata{"version", "lock_kind", "range_reads", "transport",
        "key_type", "duplicate_keys", "range_semantics", "record_lock", "gap_lock", "name"};
    const std::map<std::string, std::size_t*> integers{
        {"key_count", &config.key_count}, {"keys", &config.key_count},
        {"transaction_count", &config.transaction_count}, {"transactions", &config.transaction_count},
        {"operations_per_transaction", &config.operations_per_transaction}, {"operations", &config.operations_per_transaction},
        {"range_length", &config.range_length}, {"range", &config.range_length},
        {"threads", &config.threads}, {"duration", &config.duration_seconds}, {"duration_seconds", &config.duration_seconds},
        {"hierarchy_bucket_width", &config.hierarchy_bucket_width}, {"bucket", &config.hierarchy_bucket_width},
        {"tpcc_items_per_order", &config.tpcc_items_per_order}, {"items", &config.tpcc_items_per_order},
        {"seed", &config.seed}, {"order", &order}, {"rdma_connections", &connections}};
    std::set<std::size_t*> assigned;
    bool zipf_seen = false, port_seen = false;
    for (const auto& entry : values) {
        const auto& key = entry.first;
        const auto& value = entry.second;
        if (metadata.count(key)) continue;
        const auto number = integers.find(key);
        if (number != integers.end()) {
            if (!assigned.insert(number->second).second) throw std::invalid_argument("duplicate aliases: " + key);
            if ((key == "bucket" || key == "hierarchy_bucket_width") &&
                value.kind == ConfigScalar::String && value.text == "auto") { bucket_auto = true; continue; }
            if (value.kind != ConfigScalar::Number) throw std::invalid_argument(key + ": expected number");
            *number->second = parse_unsigned(value.text, key);
        } else if (key == "zipf" || key == "zipf_theta") {
            if (zipf_seen || value.kind != ConfigScalar::Number) throw std::invalid_argument("invalid/duplicate zipf");
            zipf_seen = true; config.zipf_theta = parse_real(value.text, key);
        } else if (key == "lock_server_port" || key == "rdma_port") {
            if (port_seen || value.kind != ConfigScalar::Number) throw std::invalid_argument("invalid/duplicate port");
            const auto parsed = parse_unsigned(value.text, key);
            if (parsed == 0 || parsed > 65535) throw std::invalid_argument("port must be 1..65535");
            port_seen = true; port = static_cast<std::uint16_t>(parsed);
        } else {
            if (value.kind != ConfigScalar::String) throw std::invalid_argument(key + ": expected string");
            if (key == "workload") config.kind = WorkloadTrace::parse_kind(value.text);
            else if (key == "index") index_name = value.text;
            else if (key == "cc") cc = value.text;
            else if (key == "rdma_host") host = value.text;
            else throw std::invalid_argument("unknown config key: " + key);
        }
    }
}

}  // namespace

int run(int argc, char** argv) {
    WorkloadConfig config;
    std::size_t order = 128;
    std::string index_name = "v2";
    std::string cc = "none";
    std::string rdma_host = "127.0.0.1";
    std::uint16_t rdma_port = 19876;
    // 0 selects the documented default (min(threads, 2)); a value forces it.
    std::size_t rdma_connections = 0;
    bool csv = false;
    bool bucket_auto = false;
    std::string config_path;
    if ((argc - 1) % 2) throw std::invalid_argument("every option requires a value");
    for (int i = 1; i + 1 < argc; i += 2)
        if (std::string(argv[i]) == "--config") {
            if (!config_path.empty()) throw std::invalid_argument("duplicate --config");
            config_path = argv[i + 1];
        }
    if (!config_path.empty()) load_config(config_path, config, index_name, cc, order,
                                         rdma_host, rdma_port, rdma_connections, bucket_auto);
    std::set<std::string> options;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        const std::string value = argv[i + 1];
        if (!options.insert(option).second) throw std::invalid_argument("duplicate option: " + option);
        if (option == "--config") continue;
        if (option == "--workload") config.kind = WorkloadTrace::parse_kind(value);
        else if (option == "--index") index_name = value;
        else if (option == "--cc") cc = value;
        else if (option == "--keys") config.key_count = parse_unsigned(value, option);
        else if (option == "--txns") config.transaction_count = parse_unsigned(value, option);
        else if (option == "--ops") config.operations_per_transaction = parse_unsigned(value, option);
        else if (option == "--range") config.range_length = parse_unsigned(value, option);
        else if (option == "--bucket") {
            if (value == "auto") bucket_auto = true;
            else { config.hierarchy_bucket_width = parse_unsigned(value, option); bucket_auto = false; }
        }
        else if (option == "--rdma-host") rdma_host = value;
        else if (option == "--rdma-port") {
            const auto parsed = parse_unsigned(value, option);
            if (parsed == 0 || parsed > 65535) throw std::invalid_argument("port must be 1..65535");
            rdma_port = static_cast<std::uint16_t>(parsed);
        }
        else if (option == "--threads") config.threads = parse_unsigned(value, option);
        else if (option == "--rdma-connections") rdma_connections = parse_unsigned(value, option);
        else if (option == "--duration") config.duration_seconds = parse_unsigned(value, option);
        else if (option == "--seed") config.seed = parse_unsigned(value, option);
        else if (option == "--zipf") config.zipf_theta = parse_real(value, option);
        else if (option == "--items") config.tpcc_items_per_order = parse_unsigned(value, option);
        else if (option == "--order") order = parse_unsigned(value, option);
        else if (option == "--format") {
            if (value != "csv" && value != "text") throw std::invalid_argument("format must be csv or text");
            csv = value == "csv";
        } else throw std::invalid_argument("unknown option: " + option);
    }

    if (index_name == "btree_latch_coupling") index_name = "v2";
    if (index_name == "btree_single") index_name = "v1";
    if (cc == "record-level-strict-2pl") cc = "record-2pl";
    const std::set<std::string> protocols{"none", "record-2pl", "gap-lock", "next-key", "hierarchical", "rdma-next-key", "rdma-hierarchical"};
    if (index_name != "v1" && index_name != "v2") throw std::invalid_argument("unknown index: " + index_name);
    if (!protocols.count(cc)) throw std::invalid_argument("unknown cc: " + cc);
    if (bucket_auto) config.hierarchy_bucket_width = auto_bucket_width(config.kind);
    if (!config.key_count || !config.transaction_count || !config.operations_per_transaction ||
        !config.threads || (config.kind == WorkloadKind::YcsbE && !config.range_length) || !config.hierarchy_bucket_width ||
        !config.tpcc_items_per_order || order < 3 || config.zipf_theta < 0)
        throw std::invalid_argument("sizes/threads/bucket/items must be positive, order >= 3, zipf >= 0");
    if (index_name == "v1" && config.threads != 1)
        throw std::invalid_argument("v1 requires exactly one thread for every cc");
    const auto max = std::numeric_limits<idx_key_t>::max();
    if (config.key_count > max - config.range_length)
        throw std::invalid_argument("key/range arithmetic overflows");
    if ((config.kind == WorkloadKind::TpccPayment || config.kind == WorkloadKind::TpccNewOrder) &&
        (config.key_count > (max - 2030000) / 30000 ||
         config.key_count > (max - 4000000) / config.key_count))
        throw std::invalid_argument("TPC-C key arithmetic overflows");
    const auto available = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::time_point::max() - std::chrono::steady_clock::now()).count();
    if (config.duration_seconds >= static_cast<std::uint64_t>(available))
        throw std::invalid_argument("duration overflows steady clock");
    in_addr address{};
    if (::inet_pton(AF_INET, rdma_host.c_str(), &address) != 1)
        throw std::invalid_argument("rdma-host must be an IPv4 address");
    const auto trace = WorkloadTrace::generate(config);
    BTreeSingle v1;
    BTreeLatch v2;
    BTreeIndex* index = nullptr;
    if (index_name == "v1") {
        if (v1.init(order) != RCOK) throw std::invalid_argument("v1 init failed");
        index = &v1;
    } else {
        if (v2.init(order) != RCOK) throw std::invalid_argument("v2 init failed");
        index = &v2;
    }

    RunStats stats;
    RemoteLockManager::Diagnostics rdma_diag{};
    // 0 on every non-RDMA protocol: no connection was opened for this run.
    std::size_t active_connections = 0;
    RdmaFailureInfo rdma_failure{};
    std::int64_t rdma_failure_shard = -1;
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
        active_connections = resolve_connection_count(config.threads, rdma_connections);
        locks.set_connection_count(active_connections);
        if (!locks.connect(rdma_host, rdma_port)) {
            std::cerr << "failed to connect RDMA lock server at " << rdma_host << ":" << rdma_port << "\n";
            return 2;
        }
        locks.set_bucket_width(config.hierarchy_bucket_width);
        if (cc == "rdma-next-key") stats = run_next_key_lock(*index, locks, trace, config.threads);
        else stats = run_hierarchical_lock(*index, locks, trace, config.threads);
        stats.rdma_timeouts = locks.rdma_timeouts();
        stats.rdma_completion_errors = locks.rdma_completion_errors();
        stats.rdma_operations = locks.rdma_operations();
        rdma_diag = locks.diagnostics();
        rdma_failure = locks.last_failure();
        rdma_failure_shard = locks.last_failure_shard();
    } else {
        stats = run_workload(*index, trace, config.threads);
    }

    if (csv) {
        std::cout << "workload,index,cc,threads,transactions,committed,aborted,operations,reads,updates,inserts,deletes,range_reads,rows_found,rows_scanned,rows_rechecked,lock_acquires,lock_conflicts,lock_capacity_aborts,lock_gate_aborts,elapsed_ns,rdma_timeouts,rdma_completion_errors,rdma_operations,throughput_committed_txn_s,throughput_attempt_txn_s,checksum,"
                  << "rdma_gate_attempts,rdma_gate_retries,rdma_gate_aborts,rdma_batches,"
                  << "rdma_failure_op,rdma_failure_shard,rdma_failure_wr,rdma_failure_us,rdma_failure_qp,"
                  << "rdma_gate_transport_errors,rdma_connections,"
                  << "hierarchy_bucket_width,lock_errors,index_errors,lock_release_failures,failed_transactions,rdma_failure_status,rdma_failure_tag,rdma_failure_address,rdma_failure_length,rdma_failure_sequence,rdma_count_errors,rdma_last_count_error,index_order\n";
        std::cout << WorkloadTrace::name(config.kind) << ',' << index_name << ',' << cc << ',' << config.threads << ','
                  << stats.transactions << ',' << stats.committed << ',' << stats.aborted << ',' << stats.operations << ','
                  << stats.reads << ',' << stats.updates << ',' << stats.inserts << ',' << stats.deletes << ','
                  << stats.range_reads << ',' << stats.rows_found << ',' << stats.rows_scanned << ',' << stats.rows_rechecked << ','
                  << stats.lock_acquires << ',' << stats.lock_conflicts << ',' << stats.lock_capacity_aborts << ',' << stats.lock_gate_aborts << ',' << stats.elapsed_ns << ','
                  << stats.rdma_timeouts << ',' << stats.rdma_completion_errors << ',' << stats.rdma_operations << ',' << stats.throughput() << ',' << stats.attempt_throughput() << ',' << stats.checksum << ','
                  << rdma_diag.gate_attempts << ',' << rdma_diag.gate_retries << ',' << rdma_diag.gate_aborts << ',' << rdma_diag.batches << ','
                  << rdma_failure.opcode << ',' << rdma_failure_shard << ',' << rdma_failure.wr_index << ',' << rdma_failure.elapsed_us << ',' << rdma_failure.qp_num << ','
                  << rdma_diag.gate_transport_errors << ','
                  << active_connections << ','
                  << config.hierarchy_bucket_width << ',' << stats.lock_errors << ',' << stats.index_errors << ','
                  << stats.lock_release_failures << ',' << stats.failed_transactions << ','
                  << rdma_failure.status << ',' << static_cast<unsigned>(rdma_failure.tag) << ','
                  << rdma_failure.remote_address << ',' << rdma_failure.length << ',' << rdma_failure.sequence << ','
                  << rdma_diag.count_errors << ',' << rdma_diag.last_count_error << ',' << order << '\n';
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
                  << ",elapsed_ns=" << stats.elapsed_ns
                  << ",rdma_timeouts=" << stats.rdma_timeouts
                  << ",rdma_completion_errors=" << stats.rdma_completion_errors
                  << ",rdma_operations=" << stats.rdma_operations
                  << ",throughput_committed_txn_s=" << stats.throughput()
                  << ",throughput_attempt_txn_s=" << stats.attempt_throughput()
                  << ",checksum=" << stats.checksum
                  << ",rdma_gate_attempts=" << rdma_diag.gate_attempts
                  << ",rdma_gate_retries=" << rdma_diag.gate_retries
                  << ",rdma_gate_aborts=" << rdma_diag.gate_aborts
                  << ",rdma_batches=" << rdma_diag.batches
                  << ",rdma_failure_op=" << rdma_failure.opcode
                  << ",rdma_failure_shard=" << rdma_failure_shard
                  << ",rdma_failure_wr=" << rdma_failure.wr_index
                  << ",rdma_failure_us=" << rdma_failure.elapsed_us
                  << ",rdma_connections=" << active_connections
                  << ",rdma_failure_qp=" << rdma_failure.qp_num
                  << ",hierarchy_bucket_width=" << config.hierarchy_bucket_width
                  << ",lock_errors=" << stats.lock_errors << ",index_errors=" << stats.index_errors
                  << ",lock_release_failures=" << stats.lock_release_failures
                  << ",failed_transactions=" << stats.failed_transactions
                  << ",rdma_failure_status=" << rdma_failure.status
                  << ",rdma_failure_tag=" << static_cast<unsigned>(rdma_failure.tag)
                  << ",rdma_failure_address=" << rdma_failure.remote_address
                  << ",rdma_failure_length=" << rdma_failure.length
                  << ",rdma_failure_sequence=" << rdma_failure.sequence
                  << ",rdma_count_errors=" << rdma_diag.count_errors
                  << ",rdma_last_count_error=" << rdma_diag.last_count_error
                  << ",index_order=" << order << "\n";
    }
    if (stats.failed() || stats.rdma_timeouts || stats.rdma_completion_errors || rdma_diag.count_errors) {
        std::cerr << "workload failed; measurement is invalid\n";
        return 3;
    }
    return 0;
}

int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& error) {
        std::cerr << "invalid configuration or setup failure: " << error.what() << "\n";
        return 2;
    }
}
