#include "txn/workload.h"
#include "index/griffin.h"
#include "index/rolex.h"
#include "index/rolex_rdma.h"
// ROLEX declarations are dependency-free; definitions are linked only when enabled.
#include "citron/citron_lock_manager.h"
#include "lock/remote_lock_manager.h"
#include "lock/object_lock_manager.h"
#include "../src/txn/leaf_hierarchy.h"
#include "../src/txn/dag_hierarchy.h"
#include "common/config_parse.h"
#include "txn/shared_dataset.h"

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <limits>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iomanip>
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
        case WorkloadKind::YcsbE:
        case WorkloadKind::YcsbESkew: return 256;
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

int derive_citron_tree_height(std::uint64_t keys) {
    std::uint64_t units = 64;
    int height = 0;
    while (units < keys + 64 && height < 15) {
        units *= 4;
        ++height;
    }
    return std::max(1, height);
}

struct IntegrationOptions {
    std::size_t citron_tree_height{0},citron_k{2},citron_m{4},citron_wait_us{50};
    std::size_t rolex_epsilon{32},rolex_segment_keys{2048},rolex_arena_leaves{0},rolex_hugepages{0};
    std::map<std::string,std::size_t*> fields() {
        return {{"citron_tree_height",&citron_tree_height},{"citron_k",&citron_k},
            {"citron_m",&citron_m},{"citron_wait_us",&citron_wait_us},
            {"rolex_epsilon",&rolex_epsilon},{"rolex_segment_keys",&rolex_segment_keys},
            {"rolex_arena_leaves",&rolex_arena_leaves},{"rolex_hugepages",&rolex_hugepages}};
    }
};

void load_config(const std::string& path, WorkloadConfig& config,
                 std::string& index_name, std::string& cc, std::size_t& order,
                 std::string& host, std::uint16_t& port, std::size_t& connections,
                 bool& bucket_auto, PolicyRunConfig& policy, bool& measured,
                 std::uint16_t& data_port, std::size_t& tree_timeout, std::size_t& griffin_queue, IntegrationOptions& extra) {
    std::ifstream input(path);
    if (!input) throw std::invalid_argument("failed to read config: " + path);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    const auto values = FlatConfigParser(text).parse();
    const std::set<std::string> metadata{"version", "lock_kind", "range_reads", "transport",
        "key_type", "duplicate_keys", "range_semantics", "record_lock", "gap_lock", "name", "tree_pages", "tree_records"};
    const std::map<std::string, std::size_t*> integers{
        {"key_count", &config.key_count}, {"keys", &config.key_count},
        {"transaction_count", &config.transaction_count}, {"transactions", &config.transaction_count},
        {"operations_per_transaction", &config.operations_per_transaction}, {"operations", &config.operations_per_transaction},
        {"range_length", &config.range_length}, {"range", &config.range_length},
        {"threads", &config.threads}, {"duration", &config.duration_seconds}, {"duration_seconds", &config.duration_seconds},
        {"hierarchy_bucket_width", &config.hierarchy_bucket_width}, {"bucket", &config.hierarchy_bucket_width},
        {"tpcc_items_per_order", &config.tpcc_items_per_order}, {"items", &config.tpcc_items_per_order},
        {"griffin_queue_capacity", &griffin_queue}, {"coroutines", &policy.coroutines}, {"coroutine_stack_bytes", &policy.coroutine_stack_bytes},
        {"warmup_seconds", &policy.warmup_seconds}, {"tree_latch_timeout_ms", &tree_timeout}, {"seed", &config.seed}, {"order", &order}, {"rdma_connections", &connections}};
    std::set<std::size_t*> assigned;
    bool zipf_seen = false, port_seen = false, distribution_seen = false;
    std::set<std::string> hotspot_seen;
    for (const auto& entry : values) {
        const auto& key = entry.first;
        const auto& value = entry.second;
        if (metadata.count(key)) continue;
        const auto extra_fields=extra.fields();
        if(const auto f=extra_fields.find(key);f!=extra_fields.end()) {
            if(value.kind!=ConfigScalar::Number)throw std::invalid_argument(key+": expected integer");
            *f->second=parse_unsigned(value.text,key);continue;
        }
        const auto number = integers.find(key);
        if (number != integers.end()) {
            if (!assigned.insert(number->second).second) throw std::invalid_argument("duplicate aliases: " + key);
            if ((key == "bucket" || key == "hierarchy_bucket_width") &&
                value.kind == ConfigScalar::String && value.text == "auto") { bucket_auto = true; continue; }
            if (value.kind != ConfigScalar::Number) throw std::invalid_argument(key + ": expected number");
            *number->second = parse_unsigned(value.text, key);
        } else if (key == "execution_mode") {
            if(value.kind!=ConfigScalar::String || (value.text!="threads" && value.text!="coroutines"))
                throw std::invalid_argument("execution_mode must be threads or coroutines");
            policy.use_coroutines=value.text=="coroutines";measured=true;
        } else if (key == "data_port") {
            if(value.kind!=ConfigScalar::Number)throw std::invalid_argument("data_port expected number");
            const auto n=parse_unsigned(value.text,key);
            if(!n || n>65535)throw std::invalid_argument("data_port bounds");
            data_port=static_cast<std::uint16_t>(n);
        } else if (key == "txn_state_port") {
            if(value.kind!=ConfigScalar::Number)throw std::invalid_argument("txn_state_port expected number");
            const auto n=parse_unsigned(value.text,key);
            if(!n || n>65535)throw std::invalid_argument("txn_state_port bounds");
            policy.txn_state_port=static_cast<std::uint16_t>(n);measured=true;
        } else if (key == "latency_histogram") {
            if (value.kind != ConfigScalar::String || value.text.empty()) throw std::invalid_argument("latency_histogram: expected nonempty path");
            policy.histogram_path = value.text; measured = true;
        } else if (key == "wait_policy") {
            if (value.kind != ConfigScalar::String) throw std::invalid_argument("wait_policy: expected string");
            policy.wait_policy = parse_wait_policy(value.text); measured = true;
        } else if (key == "max_attempts" || key == "wait_timeout_ms" || key == "coordinator_port" || key == "latency" || key == "lock_budget") {
            if (value.kind != ConfigScalar::Number) throw std::invalid_argument(key + ": expected unsigned number");
            const auto n = parse_unsigned(value.text,key);
            if (key == "max_attempts") policy.max_attempts = n;
            else if (key == "lock_budget") policy.lock_budget = n;
            else if (key == "wait_timeout_ms") policy.wait_timeout_ms = n;
            else if (key == "coordinator_port") {
                if (!n || n > 65535) throw std::invalid_argument("coordinator_port must be 1..65535");
                policy.coordinator_port = static_cast<std::uint16_t>(n);
            } else if (n > 1) throw std::invalid_argument("latency must be 0 or 1");
            if (key != "latency" || n) measured = true;
        } else if(key=="hierarchy_index_paths" || key=="hierarchy_read_path") {
            if(value.kind!=ConfigScalar::Number)throw std::invalid_argument(key+": expected integer");
            const auto n=parse_unsigned(value.text,key);
            if(key=="hierarchy_index_paths")policy.hierarchy_index_paths=n;
            else policy.hierarchy_read_path=n;
            measured=true;
        } else if(key=="hierarchy_escalation_threshold" || key=="hierarchy_deescalate_after") {
            if(value.kind!=ConfigScalar::Number)throw std::invalid_argument(key+": expected integer");
            const auto n=parse_unsigned(value.text,key);
            if(key=="hierarchy_escalation_threshold")policy.hierarchy_escalation_threshold=n;
            else policy.hierarchy_deescalate_after=n;
            measured=true;
        } else if (key == "hierarchy_behavior") {
            if(value.kind!=ConfigScalar::String || (value.text!="strict" && value.text!="deneva"))throw std::invalid_argument("hierarchy_behavior must be strict or deneva");
            policy.deneva_insert_behavior=value.text=="deneva";measured=true;
        } else if (key == "hierarchy_layout") {
            if(value.kind!=ConfigScalar::String || (value.text!="leaf" && value.text!="bucket" && value.text!="dag" && value.text!="separator"))throw std::invalid_argument("hierarchy_layout must be leaf, bucket, dag or separator");
            policy.leaf_hierarchy=value.text=="leaf";policy.dag_hierarchy=value.text=="dag";policy.separator_hierarchy=value.text=="separator";measured=true;
        } else if (key == "rdma_lock_slots_per_shard") {
            if(value.kind!=ConfigScalar::Number)throw std::invalid_argument("rdma_lock_slots_per_shard must be integer");
            const auto slots=parse_unsigned(value.text,key);
            (void)remote_lock_layout(slots);policy.rdma_lock_slots_per_shard=slots;
        } else if (key == "native_object_locks") {
            if (value.kind != ConfigScalar::Number) throw std::invalid_argument("native_object_locks: expected 0 or 1");
            const auto enabled = parse_unsigned(value.text, key);
            if (enabled > 1) throw std::invalid_argument("native_object_locks must be 0 or 1");
            policy.native_object_locks = enabled != 0; measured = true;
        } else if (key == "ycsb_unique_keys") {
            if (value.kind != ConfigScalar::Number) throw std::invalid_argument("ycsb_unique_keys: expected 0 or 1");
            const auto n = parse_unsigned(value.text,key);
            if (n > 1) throw std::invalid_argument("ycsb_unique_keys must be 0 or 1");
            config.ycsb_unique_keys = n != 0;
        } else if (key == "hierarchy_diagnostics") {
            if (value.kind != ConfigScalar::Number) throw std::invalid_argument("hierarchy_diagnostics: expected 0 or 1");
            const auto enabled = parse_unsigned(value.text, key);
            if (enabled > 1) throw std::invalid_argument("hierarchy_diagnostics must be 0 or 1");
            config.hierarchy_diagnostics = enabled != 0;
        } else if (key == "request_distribution" || key == "requestdistribution") {
            if (distribution_seen || value.kind!=ConfigScalar::String) throw std::invalid_argument("invalid/duplicate request_distribution");
            distribution_seen=true;config.request_distribution=value.text;
        } else if (key == "hotspot_data_fraction" || key == "hotspotdatafraction" ||
                   key == "hotspot_opn_fraction" || key == "hotspotopnfraction") {
            const bool data=key=="hotspot_data_fraction" || key=="hotspotdatafraction";
            if (value.kind!=ConfigScalar::Number || !hotspot_seen.insert(data?"data":"opn").second)
                throw std::invalid_argument("invalid/duplicate hotspot fraction");
            (data?config.hotspot_data_fraction:config.hotspot_opn_fraction)=parse_real(value.text,key);
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
    IntegrationOptions extra;
    PolicyRunConfig policy;
    bool measured = false;
    bool lock_only = false;
    std::size_t order = 128;
    std::string index_name = "v2";
    std::string cc = "none";
    std::string rdma_host = "127.0.0.1";
    std::uint16_t rdma_port = 19876, data_port = 0;
    std::size_t tree_timeout = 10000, griffin_queue=8192;
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
                                         rdma_host, rdma_port, rdma_connections, bucket_auto, policy, measured, data_port, tree_timeout, griffin_queue, extra);
    std::set<std::string> options;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        const std::string value = argv[i + 1];
        if (!options.insert(option).second) throw std::invalid_argument("duplicate option: " + option);
        if (option == "--config") continue;
        auto extra_name=option.substr(2);std::replace(extra_name.begin(),extra_name.end(),'-','_');
        const auto extra_fields=extra.fields();
        if(const auto f=extra_fields.find(extra_name);f!=extra_fields.end()) {
            *f->second=parse_unsigned(value,option);continue;
        }
        if (option == "--workload") config.kind = WorkloadTrace::parse_kind(value);
        else if (option == "--index") index_name = value;
        else if(option=="--griffin-queue-capacity")griffin_queue=parse_unsigned(value,option);
        else if (option == "--cc") cc = value;
        else if (option == "--latency-histogram") {
            if (value.empty()) throw std::invalid_argument("latency-histogram: expected nonempty path");
            policy.histogram_path = value; measured = true;
        }
        else if(option=="--hierarchy-index-paths" || option=="--hierarchy-read-path") {
            const auto n=parse_unsigned(value,option);
            if(option=="--hierarchy-index-paths")policy.hierarchy_index_paths=n;
            else policy.hierarchy_read_path=n;
            measured=true;
        }
        else if(option=="--hierarchy-escalation-threshold" || option=="--hierarchy-deescalate-after") {
            const auto n=parse_unsigned(value,option);
            if(option=="--hierarchy-escalation-threshold")policy.hierarchy_escalation_threshold=n;
            else policy.hierarchy_deescalate_after=n;
            measured=true;
        }
        else if(option=="--hierarchy-behavior") {
            if(value!="strict" && value!="deneva")throw std::invalid_argument("hierarchy-behavior must be strict or deneva");
            policy.deneva_insert_behavior=value=="deneva";measured=true;
        }
        else if(option=="--hierarchy-layout") {
            if(value!="leaf" && value!="bucket" && value!="dag" && value!="separator")throw std::invalid_argument("hierarchy-layout must be leaf, bucket, dag or separator");
            policy.leaf_hierarchy=value=="leaf";policy.dag_hierarchy=value=="dag";policy.separator_hierarchy=value=="separator";measured=true;
        }
        else if (option == "--rdma-lock-slots-per-shard") {
            const auto slots=parse_unsigned(value,option);
            (void)remote_lock_layout(slots);policy.rdma_lock_slots_per_shard=slots;
        }
        else if (option == "--native-object-locks") {
            const auto enabled = parse_unsigned(value, option);
            if (enabled > 1) throw std::invalid_argument("native-object-locks must be 0 or 1");
            policy.native_object_locks = enabled != 0; measured = true;
        }
        else if (option == "--wait-policy") { policy.wait_policy = parse_wait_policy(value); measured = true; }
        else if (option == "--lock-budget") { policy.lock_budget = parse_unsigned(value,option); measured = true; }
        else if (option == "--max-attempts") { policy.max_attempts = parse_unsigned(value,option); measured = true; }
        else if (option == "--wait-timeout-ms") { policy.wait_timeout_ms = parse_unsigned(value,option); measured = true; }
        else if (option == "--execution-mode") {
            if(value!="threads" && value!="coroutines")throw std::invalid_argument("execution-mode must be threads or coroutines");
            policy.use_coroutines=value=="coroutines";measured=true;
        }
        else if (option == "--coroutines") {policy.coroutines=parse_unsigned(value,option);measured=true;}
        else if (option == "--coroutine-stack-bytes") {policy.coroutine_stack_bytes=parse_unsigned(value,option);measured=true;}
        else if (option == "--warmup") {policy.warmup_seconds=parse_unsigned(value,option);measured=true;}
        else if (option == "--txn-state-port") {
            const auto n=parse_unsigned(value,option);
            if(!n || n>65535)throw std::invalid_argument("txn-state-port bounds");
            policy.txn_state_port=static_cast<std::uint16_t>(n);measured=true;
        }
        else if (option == "--coordinator-port") {
            const auto n = parse_unsigned(value,option);
            if (!n || n > 65535) throw std::invalid_argument("coordinator-port must be 1..65535");
            policy.coordinator_port = static_cast<std::uint16_t>(n); measured = true;
        }
        else if (option == "--latency") {
            const auto n = parse_unsigned(value,option);
            if (n > 1) throw std::invalid_argument("latency must be 0 or 1");
            if (n) measured = true;
        }
        else if (option == "--keys") config.key_count = parse_unsigned(value, option);
        else if (option == "--txns") config.transaction_count = parse_unsigned(value, option);
        else if (option == "--ops") config.operations_per_transaction = parse_unsigned(value, option);
        else if (option == "--ycsb-unique-keys") {
            const auto n = parse_unsigned(value,option);
            if (n > 1) throw std::invalid_argument("ycsb-unique-keys must be 0 or 1");
            config.ycsb_unique_keys = n != 0;
        }
        else if (option == "--range") config.range_length = parse_unsigned(value, option);
        else if (option == "--bucket") {
            if (value == "auto") bucket_auto = true;
            else { config.hierarchy_bucket_width = parse_unsigned(value, option); bucket_auto = false; }
        }
        else if (option == "--rdma-lock-only") {
            auto n=parse_unsigned(value,option);if(n>1)throw std::invalid_argument("rdma-lock-only must be 0 or 1");
            lock_only=n!=0;
        }
        else if (option == "--data-port") {
            auto n=parse_unsigned(value,option);if(!n || n>65535)throw std::invalid_argument("data-port bounds");
            data_port=static_cast<std::uint16_t>(n);
        }
        else if (option == "--tree-latch-timeout-ms") tree_timeout=parse_unsigned(value,option);
        else if (option == "--rdma-host") rdma_host = value;
        else if (option == "--rdma-port") {
            const auto parsed = parse_unsigned(value, option);
            if (parsed == 0 || parsed > 65535) throw std::invalid_argument("port must be 1..65535");
            rdma_port = static_cast<std::uint16_t>(parsed);
        }
        else if (option == "--threads") config.threads = parse_unsigned(value, option);
        else if (option == "--rdma-connections") rdma_connections = parse_unsigned(value, option);
        else if (option == "--hierarchy-diagnostics") {
            const auto enabled = parse_unsigned(value, option);
            if (enabled > 1) throw std::invalid_argument("hierarchy-diagnostics must be 0 or 1");
            config.hierarchy_diagnostics = enabled != 0;
        }
        else if (option == "--duration") config.duration_seconds = parse_unsigned(value, option);
        else if (option == "--seed") config.seed = parse_unsigned(value, option);
        else if (option == "--zipf") config.zipf_theta = parse_real(value, option);
        else if (option == "--request-distribution") config.request_distribution=value;
        else if (option == "--hotspot-data-fraction") config.hotspot_data_fraction=parse_real(value,option);
        else if (option == "--hotspot-opn-fraction") config.hotspot_opn_fraction=parse_real(value,option);
        else if (option == "--items") config.tpcc_items_per_order = parse_unsigned(value, option);
        else if (option == "--order") order = parse_unsigned(value, option);
        else if (option == "--format") {
            if (value != "csv" && value != "text") throw std::invalid_argument("format must be csv or text");
            csv = value == "csv";
        } else throw std::invalid_argument("unknown option: " + option);
    }

    if(extra.citron_tree_height>10 || !extra.citron_k || extra.citron_k>2 || !extra.citron_m || extra.citron_m>16 ||
       !extra.citron_wait_us || extra.citron_wait_us>1000000 || extra.rolex_hugepages>1 ||
       !extra.rolex_segment_keys || extra.rolex_segment_keys>16777216 || extra.rolex_epsilon>16777216)
        throw std::invalid_argument("Citron/ROLEX geometry bounds");
    if((cc=="citron" || cc=="rdma-citron") && (policy.wait_policy!=WaitPolicy::NoWait || policy.use_coroutines ||
       policy.coordinator_port || policy.leaf_hierarchy || policy.dag_hierarchy || policy.separator_hierarchy ||
       (cc=="citron" && lock_only)))
        throw std::invalid_argument("Citron requires no-wait and thread execution; rdma-citron lock-only uses the remote lock tree");
    if(index_name=="rolex" && (policy.leaf_hierarchy || policy.separator_hierarchy))
        throw std::invalid_argument("ROLEX does not expose B+ tree physical hierarchy metadata");
    if(index_name=="rolex" || index_name=="rolex-rdma")measured=true;
    if (index_name == "btree_latch_coupling") index_name = "v2";
    if (index_name == "btree_single") index_name = "v1";
    if (cc == "record-level-strict-2pl") cc = "record-2pl";
    const std::set<std::string> protocols{"none", "record-2pl", "gap-lock", "next-key", "prior-key", "hierarchical", "predicate", "exact-range", "citron", "rdma-citron", "rdma-next-key", "rdma-prior-key", "rdma-hierarchical", "rdma-predicate", "rdma-exact-range", "rdma-record-2pl", "rdma-gap-lock", "precision-lock", "griffin-pl", "rdma-precision-lock", "rdma-griffin-pl"};
    if (index_name != "v1" && index_name != "v2" && index_name != "blink" && index_name != "griffin" && index_name != "rolex" && index_name != "rdma" && index_name != "rolex-rdma") throw std::invalid_argument("unknown index: " + index_name);
    if(index_name=="griffin" && cc!="griffin-pl")
        throw std::invalid_argument("index griffin requires cc griffin-pl; RDMA uses the shared remote backend");
    if(policy.deneva_insert_behavior && (!policy.leaf_hierarchy || policy.separator_hierarchy || cc!="rdma-hierarchical" || index_name!="rdma" || !policy.txn_state_port))throw std::invalid_argument("deneva behavior requires native RDMA leaf hierarchy");
    if(policy.native_object_locks && (cc!="rdma-record-2pl" || index_name!="rdma" || !policy.txn_state_port || policy.leaf_hierarchy))
        throw std::invalid_argument("native object locks require rdma-record-2pl, shared RDMA data, native txn state, and no leaf hierarchy");
    if((policy.leaf_hierarchy || policy.dag_hierarchy || policy.separator_hierarchy) && cc!="hierarchical" && cc!="rdma-hierarchical")throw std::invalid_argument("hierarchy layout requires hierarchical protocol");
    if((policy.leaf_hierarchy || policy.dag_hierarchy || policy.separator_hierarchy) && policy.coordinator_port)throw std::invalid_argument("hierarchy layout requires local or native coordination");
    if((policy.hierarchy_escalation_threshold || policy.hierarchy_deescalate_after) &&
       ((cc!="hierarchical" && cc!="rdma-hierarchical") || policy.leaf_hierarchy || policy.separator_hierarchy || policy.dag_hierarchy ||
        policy.coordinator_port || policy.hierarchy_escalation_threshold>1048576 || policy.hierarchy_deescalate_after>1048576))
        throw std::invalid_argument("granularity conversion requires fixed buckets with local/native RDMA state and bounded thresholds");
    if (!protocols.count(cc)) throw std::invalid_argument("unknown cc: " + cc);
    if (cc == "precision-lock" || cc == "griffin-pl" || cc == "rdma-precision-lock" || cc == "rdma-griffin-pl" || cc == "predicate" || cc == "rdma-predicate" || cc == "exact-range" || cc == "rdma-exact-range" || cc == "citron" || cc == "rdma-citron" || policy.warmup_seconds || policy.coroutines>1) measured = true;
    if(!policy.coroutines || policy.coroutines>4096 || config.threads>4096/policy.coroutines ||
       policy.coroutine_stack_bytes<65536 || policy.coroutine_stack_bytes>16777216 || policy.warmup_seconds>600000)
        throw std::invalid_argument("runtime geometry bounds");
    if(!policy.use_coroutines && policy.coroutines!=1)
        throw std::invalid_argument("threads execution requires --coroutines 1");
    if(policy.txn_state_port && (cc.rfind("rdma-",0)!=0 || policy.coordinator_port))
        throw std::invalid_argument("native RDMA state requires rdma protocol and excludes TCP coordinator");
    if(policy.coroutines>1 && policy.coordinator_port)
        throw std::invalid_argument("coroutines require local or native RDMA state; TCP is compatibility only");
    if(cc.rfind("rdma-",0)==0 && index_name!="rdma" && index_name!="rolex-rdma" && !lock_only)
        throw std::invalid_argument("RDMA experiments require --index rdma and shared data; historical lock-service microbenchmarks require explicit --rdma-lock-only 1");
    if(lock_only && (index_name=="rdma" || cc.rfind("rdma-",0)!=0))
        throw std::invalid_argument("rdma-lock-only is only for local-index/remote-lock microbenchmarks");
    if(index_name=="rdma") {
        if(cc.rfind("rdma-",0)!=0 || !data_port || (!policy.coordinator_port && !policy.txn_state_port) || !tree_timeout || tree_timeout>600000)
            throw std::invalid_argument("shared RDMA index requires rdma protocol, data-port, txn-state-port (or legacy coordinator-port) and bounded tree timeout");
        measured=true;
    } else if(index_name=="rolex-rdma") {
        if(!data_port || !tree_timeout || tree_timeout>600000)
            throw std::invalid_argument("remote ROLEX requires data-port and bounded timeout");
        measured=true;
    } else if(data_port) throw std::invalid_argument("data-port requires --index rdma");
    if (!policy.max_attempts || !policy.wait_timeout_ms || policy.wait_timeout_ms > 600000 || !policy.lock_budget || policy.lock_budget > 1048576)
        throw std::invalid_argument("invalid max-attempts, wait-timeout-ms (1..600000) or lock-budget (1..1048576)");
    if (measured && cc == "none" && policy.wait_policy!=WaitPolicy::NoWait) throw std::invalid_argument("wait-policy/latency requires a lock protocol");
    if (measured && config.hierarchy_diagnostics) throw std::invalid_argument("legacy hierarchy diagnostics are separate from latency mode");
    if (measured && cc.rfind("rdma-",0) == 0) {
        const bool citron_lock_only = cc == "rdma-citron" && lock_only;
        if (!citron_lock_only && !policy.coordinator_port && !policy.txn_state_port) throw std::invalid_argument("RDMA policy requires --txn-state-port or explicit legacy --coordinator-port");
        if (!citron_lock_only) policy.coordinator_host = rdma_host;
    }
    if (bucket_auto) config.hierarchy_bucket_width = auto_bucket_width(config.kind);
    if(policy.dag_hierarchy) {
        if(bucket_auto)throw std::invalid_argument("DAG topology requires explicit numeric bucket width");
        detail::check_dag_geometry(config.hierarchy_bucket_width,policy.hierarchy_index_paths,policy.hierarchy_read_path);
    } else if(policy.hierarchy_index_paths!=2 || policy.hierarchy_read_path)
        throw std::invalid_argument("index paths/read path require DAG layout");
    if (!config.key_count || !config.transaction_count || !config.operations_per_transaction ||
        !config.threads || (is_ycsb_e(config.kind) && !config.range_length) || !config.hierarchy_bucket_width ||
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
    BTreeBlink blink;
    BTreeGriffin griffin(griffin_queue);
#ifdef RRL_ENABLE_ROLEX
    rolex::port::RolexIndex rolex_index(extra.rolex_epsilon,64,extra.rolex_hugepages!=0,extra.rolex_segment_keys,extra.rolex_arena_leaves);
#endif
    BTreeRdma remote_index;
    rolex::remote::RolexRdmaIndex remote_rolex_index;
    BTreeIndex* index = nullptr;
    if (index_name == "v1") {
        if (v1.init(order) != RCOK) throw std::invalid_argument("v1 init failed");
        index = &v1;
    } else if (index_name == "griffin") {
        if (griffin.init(order) != RCOK) throw std::invalid_argument("griffin init failed");
        index = &griffin;
    } else if (index_name == "rolex") {
#ifdef RRL_ENABLE_ROLEX
        if (rolex_index.init(order) != RCOK) throw std::invalid_argument("rolex init failed");
        index = &rolex_index;
#else
        throw std::invalid_argument("index rolex requires CMake RRL_ENABLE_ROLEX=ON (GPL-3.0)");
#endif
    } else if (index_name == "blink") {
        if (blink.init(order) != RCOK) throw std::invalid_argument("blink init failed");
        index = &blink;
    } else if(index_name=="rolex-rdma") {
        const auto seed=make_shared_seed(trace);
        if(!data_port) throw std::invalid_argument("index rolex-rdma requires data_port");
        if(!remote_rolex_index.connect(rdma_host,data_port,std::min(config.threads*policy.coroutines,config.transaction_count),rolex::remote::seed_fingerprint(seed),tree_timeout) || remote_rolex_index.init(order)!=RCOK)
            throw std::runtime_error("remote ROLEX RDMA connect/configuration mismatch");
        index=&remote_rolex_index;
    } else if(index_name=="rdma") {
        const auto seed=make_shared_seed(trace);
        if(!remote_index.connect(rdma_host,data_port,std::min(config.threads*policy.coroutines,config.transaction_count),tree_seed_fingerprint(seed),tree_timeout) ||
           remote_index.init(order)!=RCOK)throw std::runtime_error("shared RDMA data connect/configuration mismatch");
        index=&remote_index;
    } else {
        if (v2.init(order) != RCOK) throw std::invalid_argument("v2 init failed");
        index = &v2;
    }

    auto data_ops_value=[&]{return index_name=="rolex-rdma"?remote_rolex_index.rdma_operations():remote_index.rdma_operations();};
    auto data_reads_value=[&]{return index_name=="rolex-rdma"?remote_rolex_index.read_bytes():remote_index.read_bytes();};
    auto data_writes_value=[&]{return index_name=="rolex-rdma"?remote_rolex_index.write_bytes():remote_index.write_bytes();};
    auto data_ops_start=data_ops_value();
    auto data_reads_start=data_reads_value(),data_writes_start=data_writes_value();
    auto data_wait_start=index_name=="rolex-rdma"?std::uint64_t{0}:remote_index.latch_wait_ns();
    auto snapshot_data=[&] {
        // Workers are quiescent here; drain warm-up replication before the
        // timed window and reset diagnostics so no warm-up events leak in.
#ifdef RRL_ENABLE_ROLEX
        if(index_name=="rolex")rolex_index.reset_diagnostics();
#endif
        data_ops_start=data_ops_value();
        data_reads_start=data_reads_value();data_writes_start=data_writes_value();
        data_wait_start=index_name=="rolex-rdma"?std::uint64_t{0}:remote_index.latch_wait_ns();
    };
    policy.measurement_begin=snapshot_data;
    RunStats stats;
    RemoteLockManager::Diagnostics rdma_diag{};
    citron::CitronLockManager::Diagnostics citron_diag{};
    rolex::port::RolexDiagnostics rolex_diag{};
    // 0 on every non-RDMA protocol: no connection was opened for this run.
    std::size_t active_connections = 0;
    RdmaFailureInfo rdma_failure{};
    std::int64_t rdma_failure_shard = -1;
    if (measured) {
        const auto plain = cc.rfind("rdma-",0) == 0 ? cc.substr(5) : cc;
        policy.protocol = plain == "none" ? PolicyProtocol::None : plain == "record-2pl" ? PolicyProtocol::Record : plain == "gap-lock" ? PolicyProtocol::Gap :
            plain == "next-key" ? PolicyProtocol::NextKey : plain == "prior-key" ? PolicyProtocol::PriorKey : plain == "predicate" ? PolicyProtocol::Predicate :
            plain == "exact-range" ? PolicyProtocol::ExactRange : plain == "precision-lock" ? PolicyProtocol::Precision :
            plain == "griffin-pl" ? PolicyProtocol::Griffin : plain == "citron" ? PolicyProtocol::Predicate : PolicyProtocol::Hierarchy;
        if (cc == "citron" || cc == "rdma-citron") {
            if (policy.wait_policy != WaitPolicy::NoWait)
                throw std::invalid_argument("Citron currently supports no-wait plus transaction retry only");
            citron::LockManagerConfig citron_config;
            citron_config.server_host = cc == "rdma-citron" ? rdma_host : std::string{};
            citron_config.server_port = rdma_port;
            citron_config.tree_height = extra.citron_tree_height ? static_cast<int>(extra.citron_tree_height) : derive_citron_tree_height(trace.max_key());
            if(trace.max_key()>=citron::SegmentTree(citron_config.tree_height).units())
                throw std::invalid_argument("Citron tree capacity does not cover workload keys");
            citron_config.lock_budget=policy.lock_budget;
            citron_config.client.k=extra.citron_k;citron_config.client.m=extra.citron_m;
            citron_config.max_sessions = std::max<std::size_t>(1, config.threads * policy.coroutines);
            citron_config.client.t_wait_ns = extra.citron_wait_us*1000;
            citron_config.client.lock_deadline_ns = std::max<std::uint64_t>(20000000,citron_config.client.t_wait_ns);
            citron_config.client.leaf_starvation_ns = 2000000;
            citron_config.no_wait = true;
            citron::CitronLockManager locks(citron_config);
            std::uint64_t citron_ops_start=0;
            policy.measurement_begin=[&]{snapshot_data();locks.reset_diagnostics();citron_ops_start=locks.diagnostics().rdma_operations;};
            stats = run_policy_workload(*index, locks, trace, policy);
            citron_diag = locks.diagnostics();citron_diag.rdma_operations-=citron_ops_start;
            stats.rdma_operations=citron_diag.rdma_operations;
            if(cc=="rdma-citron")active_connections=citron_diag.sessions;
            if(locks.get_lock_count())++stats.lock_release_failures;
        } else if(policy.leaf_hierarchy || policy.dag_hierarchy || policy.native_object_locks) {
            ObjectLockManager locks;
            if(policy.dag_hierarchy && !locks.configure_dag(config.hierarchy_bucket_width,policy.hierarchy_index_paths))
                throw std::invalid_argument("DAG object topology");
            const auto keys=detail::object_record_keys(trace);
            const auto contexts=std::min(config.threads*policy.coroutines,config.transaction_count);
            if(cc.rfind("rdma-",0)==0) {
                if(!policy.txn_state_port)throw std::invalid_argument("RDMA leaf layout requires txn-state-port");
                if(!locks.connect(rdma_host,rdma_port,keys,contexts))throw std::runtime_error("object RDMA connect/config mismatch");
                active_connections=contexts;
            }else {
                std::size_t operations=0;for(const auto& t:trace.transactions())operations+=t.operations.size();
                if(keys.size()>(SIZE_MAX-operations-16)/2)throw std::invalid_argument("local leaf directory size");
                if(!locks.init_local(keys,policy.dag_hierarchy?detail::dag_node_capacity(trace,config.hierarchy_bucket_width,policy.hierarchy_index_paths):
                    2*(keys.size()+operations)+16,contexts,policy.lock_budget))throw std::runtime_error("local object init");
            }
            std::uint64_t start_ops=0;
            policy.measurement_begin=[&]{snapshot_data();start_ops=locks.rdma_operations();};
            stats=run_policy_workload(*index,locks,trace,policy);
            stats.rdma_operations=locks.rdma_operations()-start_ops;
            stats.rdma_timeouts=locks.rdma_timeouts();stats.rdma_completion_errors=locks.rdma_completion_errors();
        }else if (cc.rfind("rdma-",0) == 0) {
            RemoteLockManager locks;
            locks.use_coordinator_transaction_ids();
            active_connections = policy.coroutines>1 && !rdma_connections ?
                std::min(config.threads*policy.coroutines,config.transaction_count) :
                resolve_connection_count(config.threads,rdma_connections);
            locks.set_connection_count(active_connections);
            locks.set_bucket_width(config.hierarchy_bucket_width);
            if (!locks.connect(rdma_host,rdma_port)) throw std::runtime_error("RDMA range lock connect/layout mismatch");
            if(policy.rdma_lock_slots_per_shard && locks.diagnostics().slots_per_shard!=policy.rdma_lock_slots_per_shard)
                throw std::runtime_error("RDMA range lock capacity differs from configuration");
            RemoteLockManager::Diagnostics before{};
            std::uint64_t ops_before=0;
            policy.measurement_begin=[&] {snapshot_data();locks.reset_slot_diagnostics();before=locks.diagnostics();ops_before=locks.rdma_operations();};
            stats = run_policy_workload(*index,locks,trace,policy);
            stats.rdma_timeouts=locks.rdma_timeouts(); stats.rdma_completion_errors=locks.rdma_completion_errors();
            stats.rdma_operations=locks.rdma_operations()-ops_before; rdma_diag=locks.diagnostics();
#define DELTA(f) rdma_diag.f-=before.f
            DELTA(gate_attempts);DELTA(gate_retries);DELTA(gate_aborts);DELTA(batches);
            DELTA(lock_calls);DELTA(cache_hits);DELTA(io_wait_ns);DELTA(local_gate_wait_ns);DELTA(gate_hold_ns);
            DELTA(index_build_ns);DELTA(lookup_ns);DELTA(slot_checks);DELTA(txn_wait_ns);
            DELTA(sync_calls);DELTA(sync_ns);DELTA(read_bytes);DELTA(write_bytes);
#undef DELTA
            for(std::size_t i=0;i<rdma_diag.phase_calls.size();++i) {
                rdma_diag.phase_calls[i]-=before.phase_calls[i];rdma_diag.phase_ns[i]-=before.phase_ns[i];
            }
            rdma_failure=locks.last_failure(); rdma_failure_shard=locks.last_failure_shard();
        } else {
            LockManager locks; locks.set_bucket_width(config.hierarchy_bucket_width);
            stats = run_policy_workload(*index,locks,trace,policy);
        }
    } else if (cc == "record-2pl") {
        LockManager locks; stats = run_record_2pl(*index, locks, trace, config.threads);
    } else if (cc == "gap-lock") {
        LockManager locks; stats = run_gap_lock(*index, locks, trace, config.threads);
    } else if (cc == "next-key" || cc == "prior-key") {
        LockManager locks;
        if(cc=="next-key")stats=run_next_key_lock(*index,locks,trace,config.threads);
        else {policy.protocol=PolicyProtocol::PriorKey;policy.max_attempts=1;policy.use_coroutines=false;
              stats=run_policy_workload(*index,locks,trace,policy);}
    } else if (cc == "hierarchical") {
        LockManager locks;
        locks.set_bucket_width(config.hierarchy_bucket_width);
        stats = run_hierarchical_lock(*index, locks, trace, config.threads);
    } else if (cc == "rdma-next-key" || cc == "rdma-prior-key" || cc == "rdma-hierarchical") {
        RemoteLockManager locks;
        active_connections = resolve_connection_count(config.threads, rdma_connections);
        locks.set_connection_count(active_connections);
        if (!locks.connect(rdma_host, rdma_port)) {
            std::cerr << "failed to connect RDMA lock server at " << rdma_host << ":" << rdma_port << "\n";
            return 2;
        }
        locks.set_bucket_width(config.hierarchy_bucket_width);
        if (cc == "rdma-next-key") stats = run_next_key_lock(*index, locks, trace, config.threads);
        else if(cc=="rdma-prior-key") {policy.protocol=PolicyProtocol::PriorKey;policy.max_attempts=1;policy.use_coroutines=false;
            stats=run_policy_workload(*index,locks,trace,policy);}
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

    // Snapshot before validate() drains the remaining replication queue.
    // Quiescent structure validation is outside the recorded workload window.
    const auto data_ops=data_ops_value()-data_ops_start;
    const auto data_reads=data_reads_value()-data_reads_start,data_writes=data_writes_value()-data_writes_start;
    const auto data_wait=index_name=="rolex-rdma"?std::uint64_t{0}:remote_index.latch_wait_ns()-data_wait_start;
    if (measured && !(index_name=="rdma" ? remote_index.healthy() && remote_index.validate() : index_name=="rolex-rdma" ? remote_rolex_index.healthy() && remote_rolex_index.validate() :
                     index_name=="v1" ? v1.validate() :
                     index_name=="blink" ? blink.validate() : index_name=="griffin" ? griffin.validate() :
#ifdef RRL_ENABLE_ROLEX
                     index_name=="rolex" ? rolex_index.validate() :
#endif
                     v2.validate())) ++stats.index_errors;
#ifdef RRL_ENABLE_ROLEX
    if (index_name == "rolex") rolex_diag = rolex_index.diagnostics();
#endif

    if (measured && !csv) {
        const auto& m=stats.policy;
        std::cout << "wait_policy=" << wait_policy_name(policy.wait_policy) << ",logical_started=" << m.logical_started
            << ",logical_unfinished=" << m.unfinished << ",retries=" << m.retries
            << ",successful_attempt_p99_ns=" << m.successful_attempt.p99_ns
            << ",logical_completion_p99_ns=" << m.logical_completion.p99_ns
            << ",lock_wait_ns=" << m.lock_wait.total_ns << "\n";
    }
    if (csv) {
        std::cout << "workload,index,cc,threads,transactions,committed,aborted,operations,reads,updates,inserts,deletes,range_reads,rows_found,rows_scanned,rows_rechecked,lock_acquires,lock_conflicts,lock_capacity_aborts,lock_gate_aborts,elapsed_ns,rdma_timeouts,rdma_completion_errors,rdma_operations,throughput_committed_txn_s,throughput_attempt_txn_s,checksum,"
                  << "rdma_gate_attempts,rdma_gate_retries,rdma_gate_aborts,rdma_batches,"
                  << "rdma_failure_op,rdma_failure_shard,rdma_failure_wr,rdma_failure_us,rdma_failure_qp,"
                  << "rdma_gate_transport_errors,rdma_connections,"
                  << "hierarchy_bucket_width,lock_errors,index_errors,lock_release_failures,failed_transactions,rdma_failure_status,rdma_failure_tag,rdma_failure_address,rdma_failure_length,rdma_failure_sequence,rdma_count_errors,rdma_last_count_error,index_order,rdma_local_errors,rdma_profile_enabled,rdma_slot_lookup_linear,rdma_lock_calls,rdma_cache_hits,rdma_io_wait_ns,rdma_local_gate_wait_ns,rdma_gate_hold_ns,rdma_index_build_ns,rdma_lookup_ns,rdma_slot_checks,rdma_sync_calls,rdma_sync_ns,rdma_read_bytes,rdma_write_bytes,rdma_none_calls,rdma_none_ns,rdma_gate_calls,rdma_gate_ns,rdma_counts_calls,rdma_counts_ns,rdma_slots_calls,rdma_slots_ns,rdma_store_calls,rdma_store_ns,rdma_release_calls,rdma_release_ns,rdma_txn_wait_ns";
        if (measured) std::cout << ",metrics_schema,wait_policy,coordination,logical_started,logical_unfinished,logical_failed,retries,max_retries,wound_aborts,deadline_aborts,retry_exhausted,completed_retries,unfinished_retries";
        if (measured) for (const char* name : {"successful_attempt","logical_completion","lock_wait","lock_call","release"})
            for (const char* field : {"count","total_ns","p50_ns","p95_ns","p99_ns","p999_ns","max_ns"}) std::cout << "," << name << "_" << field;
        std::cout << ",data_backend,data_rdma_operations,data_read_bytes,data_write_bytes,data_latch_wait_ns,coroutines,active_contexts,warmup_seconds,warmup_committed,warmup_aborted,warmup_elapsed_ns,coroutine_switches_total,state_rdma_operations,hierarchy_layout,native_object_locks,hierarchy_behavior,isolation_semantics,deneva_early_inserts,deneva_early_unlocks,deneva_split_aborts,deneva_partial_write_aborts,execution_mode";
        std::cout << ",hierarchy_index_paths,hierarchy_read_path";
        std::cout << ",hierarchy_escalation_threshold,hierarchy_deescalate_after,escalation_attempts,escalations,escalation_failures,deescalation_attempts,deescalations,deescalation_failures";
        std::cout << ",trace_zipf_theta,trace_key_count,trace_transaction_count,trace_operations_per_transaction,trace_range_length,trace_seed,trace_ycsb_unique_keys,trace_key_distribution,trace_scan_semantics,trace_request_distribution,trace_hotspot_data_fraction,trace_hotspot_opn_fraction";
        std::cout << ",rdma_lock_slots_per_shard";
        for(std::size_t shard=0;shard<kRemoteLockShards;++shard)
            std::cout << ",rdma_lock_slot_peak_" << shard << ",rdma_lock_capacity_required_" << shard;
        std::cout << ",index_sync_pending_aborts";
        std::cout << ",active_workers,workers_without_commit,worker_min_committed,worker_max_committed";
        std::cout << ",citron_lock_calls,citron_grants,citron_aborts,citron_errors,citron_unlock_calls,citron_unlock_failures,citron_rdma_operations,citron_sessions,citron_shared_mapped_to_exclusive";
        std::cout << ",rolex_segments,rolex_leaves,rolex_live_keys,rolex_splits,rolex_removals,rolex_model_exact,rolex_model_corrected,rolex_model_fallbacks,rolex_arena_bytes,rolex_build_ns,rolex_mapping";
        std::cout << ",citron_tree_height,citron_k,citron_m,citron_wait_us,citron_lock_semantics,rolex_epsilon,rolex_segment_keys,rolex_arena_leaves,rolex_hugepages";
        std::cout << '\n';
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
                  << rdma_diag.count_errors << ',' << rdma_diag.last_count_error << ',' << order
                  << ',' << rdma_diag.local_errors
                  << ',' << rdma_diag.profile_enabled
                  << ',' << rdma_diag.slot_lookup_linear
                  << ',' << rdma_diag.lock_calls
                  << ',' << rdma_diag.cache_hits
                  << ',' << rdma_diag.io_wait_ns
                  << ',' << rdma_diag.local_gate_wait_ns
                  << ',' << rdma_diag.gate_hold_ns
                  << ',' << rdma_diag.index_build_ns
                  << ',' << rdma_diag.lookup_ns
                  << ',' << rdma_diag.slot_checks
                  << ',' << rdma_diag.sync_calls
                  << ',' << rdma_diag.sync_ns
                  << ',' << rdma_diag.read_bytes
                  << ',' << rdma_diag.write_bytes;
        for (std::size_t i = 0; i < rdma_diag.phase_calls.size(); ++i)
            std::cout << ',' << rdma_diag.phase_calls[i] << ',' << rdma_diag.phase_ns[i];
        std::cout << ',' << rdma_diag.txn_wait_ns;
        if (measured) {
            const auto& m=stats.policy;
            std::cout << ",2," << wait_policy_name(policy.wait_policy) << ',' << (policy.txn_state_port?"native-rdma-state":policy.coordinator_host.empty()?"native-local-state":"tcp-control+rdma-slots")
                << ',' << m.logical_started << ',' << m.unfinished << ',' << m.logical_failed << ',' << m.retries << ',' << m.max_retries
                << ',' << m.wound_aborts << ',' << m.deadline_aborts << ',' << m.retry_exhausted << ',' << m.completed_retries << ',' << m.unfinished_retries;
            for (const auto* q : {&m.successful_attempt,&m.logical_completion,&m.lock_wait,&m.lock_call,&m.release})
                std::cout << ',' << q->count << ',' << q->total_ns << ',' << q->p50_ns << ',' << q->p95_ns << ',' << q->p99_ns << ',' << q->p999_ns << ',' << q->max_ns;
        }
        std::cout << ',' << (index_name=="rdma"?"shared-rdma":index_name=="rolex-rdma"?"remote-rolex-rdma":"local") << ',' << data_ops << ',' << data_reads
                  << ',' << data_writes << ',' << data_wait
                  << ',' << policy.coroutines << ',' << std::min(config.threads*policy.coroutines,config.transaction_count)
                  << ',' << policy.warmup_seconds << ',' << stats.policy.warmup_committed << ',' << stats.policy.warmup_aborted
                  << ',' << stats.policy.warmup_elapsed_ns << ',' << stats.policy.coroutine_switches
                  << ',' << stats.policy.state_rdma_operations << ',' << ((cc=="hierarchical" || cc=="rdma-hierarchical")?(policy.leaf_hierarchy?"leaf":policy.dag_hierarchy?"dag":policy.separator_hierarchy?"separator":"bucket"):"none")
                  << ',' << (policy.native_object_locks?1:0)
                  << ',' << (policy.deneva_insert_behavior?"deneva":"strict") << ',' << (policy.deneva_insert_behavior?"deneva-early-release":"strict-2pl")
                  << ',' << stats.policy.deneva_early_inserts << ',' << stats.policy.deneva_early_unlocks
                  << ',' << stats.policy.deneva_split_aborts << ',' << stats.policy.deneva_partial_write_aborts
                  << ',' << (measured?(policy.use_coroutines?"coroutines":"threads"):"threads");
        std::cout << ',' << (policy.dag_hierarchy?policy.hierarchy_index_paths:0) << ',' << (policy.dag_hierarchy?policy.hierarchy_read_path:0);
        std::cout << ',' << policy.hierarchy_escalation_threshold << ',' << policy.hierarchy_deescalate_after
            << ',' << stats.policy.escalation_attempts << ',' << stats.policy.escalations << ',' << stats.policy.escalation_failures
            << ',' << stats.policy.deescalation_attempts << ',' << stats.policy.deescalations << ',' << stats.policy.deescalation_failures;
        const bool ycsb = config.kind == WorkloadKind::YcsbA || config.kind == WorkloadKind::YcsbB || is_ycsb_e(config.kind);
        std::cout << ',' << std::setprecision(17) << config.zipf_theta << ',' << config.key_count
                  << ',' << config.transaction_count << ',' << config.operations_per_transaction
                  << ',' << config.range_length << ',' << config.seed << ',' << config.ycsb_unique_keys
                  << ',' << (ycsb?WorkloadTrace::key_distribution(config):"not-applicable")
                  << ',' << (is_ycsb_e(config.kind)?"fixed-key-width-clipped":"not-applicable")
                  << ',' << config.request_distribution << ',' << config.hotspot_data_fraction << ',' << config.hotspot_opn_fraction;
        std::cout << ',' << rdma_diag.slots_per_shard;
        for(std::size_t shard=0;shard<kRemoteLockShards;++shard)
            std::cout << ',' << rdma_diag.slot_peaks[shard] << ',' << rdma_diag.capacity_required[shard];
        std::cout << ',' << stats.policy.index_pending_aborts;
        std::cout << ',' << stats.policy.active_workers << ',' << stats.policy.workers_without_commit << ',' << stats.policy.worker_min_committed << ',' << stats.policy.worker_max_committed;
        std::cout << ',' << citron_diag.lock_calls << ',' << citron_diag.lock_grants << ',' << citron_diag.lock_aborts
                  << ',' << citron_diag.lock_errors << ',' << citron_diag.unlock_calls << ',' << citron_diag.unlock_failures
                  << ',' << citron_diag.rdma_operations << ',' << citron_diag.sessions << ',' << citron_diag.shared_mapped_to_exclusive;
        std::cout << ',' << rolex_diag.segments << ',' << rolex_diag.leaf_allocations << ',' << rolex_diag.live_keys
                  << ',' << rolex_diag.leaf_splits << ',' << rolex_diag.leaf_removals << ',' << rolex_diag.model_exact_pages
                  << ',' << rolex_diag.model_corrected_pages << ',' << rolex_diag.model_fallbacks << ',' << rolex_diag.arena_bytes
                  << ',' << rolex_diag.build_ns << ',' << rolex_diag.mapping;
        const bool citron_run=cc=="citron" || cc=="rdma-citron";
        std::cout << ',' << (citron_run?(extra.citron_tree_height?extra.citron_tree_height:derive_citron_tree_height(trace.max_key())):0)
                  << ',' << (citron_run?extra.citron_k:0) << ',' << (citron_run?extra.citron_m:0)
                  << ',' << (citron_run?extra.citron_wait_us:0) << ',' << (citron_run?"exclusive-range-S-as-X":"none")
                  << ',' << (index_name=="rolex"?extra.rolex_epsilon:0) << ',' << (index_name=="rolex"?extra.rolex_segment_keys:0)
                  << ',' << (index_name=="rolex"?extra.rolex_arena_leaves:0) << ',' << (index_name=="rolex"?extra.rolex_hugepages:0);
        std::cout << '\n';
    } else {
        std::cout<<"active_workers="<<stats.policy.active_workers<<",workers_without_commit="<<stats.policy.workers_without_commit<<",worker_min_committed="<<stats.policy.worker_min_committed<<",worker_max_committed="<<stats.policy.worker_max_committed<<"\n";
        std::cout << "index_sync_pending_aborts=" << stats.policy.index_pending_aborts << "\n";
        if (cc == "citron" || cc == "rdma-citron")
            std::cout << "citron_lock_calls=" << citron_diag.lock_calls
                      << ",citron_grants=" << citron_diag.lock_grants
                      << ",citron_aborts=" << citron_diag.lock_aborts
                      << ",citron_errors=" << citron_diag.lock_errors
                      << ",citron_unlock_failures=" << citron_diag.unlock_failures
                      << ",citron_rdma_operations=" << citron_diag.rdma_operations
                      << ",citron_sessions=" << citron_diag.sessions << "\n";
        if (index_name == "rolex")
            std::cout << "rolex_segments=" << rolex_diag.segments
                      << ",rolex_leaves=" << rolex_diag.leaf_allocations
                      << ",rolex_live_keys=" << rolex_diag.live_keys
                      << ",rolex_splits=" << rolex_diag.leaf_splits
                      << ",rolex_model_fallbacks=" << rolex_diag.model_fallbacks
                      << ",rolex_build_ns=" << rolex_diag.build_ns
                      << ",rolex_mapping=" << rolex_diag.mapping << "\n";
        std::cout << "execution_mode=" << (measured?(policy.use_coroutines?"coroutines":"threads"):"threads") << "\n";
        std::cout << "data_backend=" << (index_name=="rdma"?"shared-rdma":index_name=="rolex-rdma"?"remote-rolex-rdma":"local")
                  << ",data_rdma_operations=" << data_ops << ",data_read_bytes=" << data_reads
                  << ",data_write_bytes=" << data_writes << ",data_latch_wait_ns=" << data_wait
                  << ",hierarchy_behavior=" << (policy.deneva_insert_behavior?"deneva":"strict")
                  << ",isolation_semantics=" << (policy.deneva_insert_behavior?"deneva-early-release":"strict-2pl") << "\n";
        std::cout << "workload=" << WorkloadTrace::name(config.kind)
                  << ",request_distribution=" << WorkloadTrace::key_distribution(config)
                  << ",hotspot_data_fraction=" << config.hotspot_data_fraction << ",hotspot_opn_fraction=" << config.hotspot_opn_fraction
                  << ",zipf_theta=" << std::setprecision(17) << config.zipf_theta
                  << ",keys=" << config.key_count << ",trace_txns=" << config.transaction_count
                  << ",ops_per_txn=" << config.operations_per_transaction << ",range=" << config.range_length
                  << ",seed=" << config.seed << ",ycsb_unique_keys=" << config.ycsb_unique_keys
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
                  << ",index_order=" << order
                  << ",rdma_local_errors=" << rdma_diag.local_errors
                  << ",rdma_profile_enabled=" << rdma_diag.profile_enabled
                  << ",rdma_slot_lookup_linear=" << rdma_diag.slot_lookup_linear
                  << ",rdma_lock_calls=" << rdma_diag.lock_calls
                  << ",rdma_cache_hits=" << rdma_diag.cache_hits
                  << ",rdma_io_wait_ns=" << rdma_diag.io_wait_ns
                  << ",rdma_local_gate_wait_ns=" << rdma_diag.local_gate_wait_ns
                  << ",rdma_gate_hold_ns=" << rdma_diag.gate_hold_ns
                  << ",rdma_index_build_ns=" << rdma_diag.index_build_ns
                  << ",rdma_lookup_ns=" << rdma_diag.lookup_ns
                  << ",rdma_slot_checks=" << rdma_diag.slot_checks
                  << ",rdma_sync_calls=" << rdma_diag.sync_calls
                  << ",rdma_sync_ns=" << rdma_diag.sync_ns
                  << ",rdma_read_bytes=" << rdma_diag.read_bytes
                  << ",rdma_write_bytes=" << rdma_diag.write_bytes
                  << ",rdma_txn_wait_ns=" << rdma_diag.txn_wait_ns << "\n";
        for (std::size_t i = 0; i < rdma_diag.phase_calls.size(); ++i)
            std::cout << "rdma_phase=" << i << ",calls=" << rdma_diag.phase_calls[i]
                      << ",ns=" << rdma_diag.phase_ns[i] << "\n";
    }
    if (stats.failed() || stats.rdma_timeouts || stats.rdma_completion_errors || rdma_diag.count_errors || rdma_diag.local_errors) {
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
