#include "txn/shared_dataset.h"
#include "common/config_parse.h"
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>

namespace rrl {
bool seed_index(BTreeIndex&,const WorkloadTrace&,std::unordered_map<idx_key_t,std::uint64_t>&);
TreeSeed make_shared_seed(const WorkloadTrace& trace) {
    BTreeSingle tree;
    if(tree.init(128)!=RCOK)throw std::runtime_error("seed tree init");
    std::unordered_map<idx_key_t,std::uint64_t> values;
    if(!seed_index(tree,trace,values))throw std::runtime_error("seed tree populate");
    TreeSeed seed;seed.reserve(values.size());
    for(const auto& p:values)seed.push_back(p);
    std::sort(seed.begin(),seed.end(),[](const auto& a,const auto& b){return a.first<b.first;});return seed;
}
void load_shared_dataset_config(const std::string& path,WorkloadConfig& c,RdmaTreeConfig& t,std::uint16_t& port) {
    std::ifstream in(path);if(!in)throw std::invalid_argument("cannot read dataset config");
    const std::string text((std::istreambuf_iterator<char>(in)),{});
    const auto entries=FlatConfigParser(text).parse();
    const std::map<std::string,std::size_t*> numbers{
        {"keys",&c.key_count},{"key_count",&c.key_count},
        {"txns",&c.transaction_count},{"transaction_count",&c.transaction_count},
        {"operations",&c.operations_per_transaction},{"operations_per_transaction",&c.operations_per_transaction},
        {"range",&c.range_length},{"range_length",&c.range_length},
        {"tpcc_items_per_order",&c.tpcc_items_per_order},{"items",&c.tpcc_items_per_order},
        {"seed",&c.seed},{"order",&t.order},{"tree_pages",&t.page_capacity},{"tree_records",&t.record_capacity},
        {"tree_latch_timeout_ms",&t.latch_timeout_ms}};
    const std::set<std::string> ignored{"name","index","cc","threads","duration","duration_seconds",
        "hierarchy_bucket_width","bucket","rdma_host","rdma_port","lock_server_port","rdma_connections",
        "coordinator_port","txn_state_port","coroutines","coroutine_stack_bytes","warmup_seconds","wait_policy","wait_timeout_ms","max_attempts","lock_budget","latency",
        "latency_histogram","hierarchy_diagnostics"};
    std::set<std::size_t*> assigned;
    bool zipf=false;
    for(const auto& e:entries) {
        auto found=numbers.find(e.first);
        if(found!=numbers.end()) {
            if(e.second.kind!=ConfigScalar::Number || !assigned.insert(found->second).second)throw std::invalid_argument("invalid/duplicate dataset integer");
            *found->second=parse_unsigned(e.second.text,e.first);
        } else if(e.first=="workload") {
            if(e.second.kind!=ConfigScalar::String)throw std::invalid_argument("workload must be string");
            c.kind=WorkloadTrace::parse_kind(e.second.text);
        } else if(e.first=="zipf" || e.first=="zipf_theta") {
            if(zipf || e.second.kind!=ConfigScalar::Number)throw std::invalid_argument("invalid/duplicate zipf");
            zipf=true;c.zipf_theta=parse_real(e.second.text,e.first);
        } else if(e.first=="data_port") {
            if(e.second.kind!=ConfigScalar::Number)throw std::invalid_argument("invalid data_port");
            auto n=parse_unsigned(e.second.text,e.first);if(!n || n>65535)throw std::invalid_argument("data_port bounds");
            port=static_cast<std::uint16_t>(n);
        } else if(!ignored.count(e.first))throw std::invalid_argument("unknown shared config key: "+e.first);
    }
    if(!c.key_count || !c.transaction_count || !c.operations_per_transaction || !c.tpcc_items_per_order ||
       !t.latch_timeout_ms || t.latch_timeout_ms>600000 || c.zipf_theta<0 ||
       c.key_count>UINT64_MAX-c.range_length)throw std::invalid_argument("invalid dataset sizes");
    if((c.kind==WorkloadKind::TpccPayment || c.kind==WorkloadKind::TpccNewOrder) &&
       (c.key_count>(UINT64_MAX-2030000)/30000 || c.key_count>(UINT64_MAX-4000000)/c.key_count))
        throw std::invalid_argument("TPC-C seed key overflow");
}
} // namespace rrl
