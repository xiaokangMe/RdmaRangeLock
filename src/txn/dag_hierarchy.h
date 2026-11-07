#pragma once
#include "hierarchical_plan.h"
#include <set>
namespace rrl::detail {
struct DagResource {
    std::uint64_t id;
    std::vector<std::uint64_t> parents;
    LockKind kind{LockKind::Node};
    std::uint64_t object_key{id};
};
// Explicit topology for reproduction/contract tests. Single root, canonical
// physical resource names, no cycles/unknown/duplicate parents. All topology
// work and plan allocations precede transactional lock acquisition.
class LockDag {
    std::map<std::uint64_t,DagResource> nodes_;
    std::vector<std::uint64_t> order_;
    std::map<std::uint64_t,unsigned> depth_;
    std::uint64_t root_{0};
public:
    explicit LockDag(std::vector<DagResource> nodes) {
        if(nodes.empty())throw std::invalid_argument("empty DAG");
        std::set<std::pair<LockKind,std::uint64_t>> names;
        for(auto& node:nodes) {
            if((node.kind!=LockKind::Node && node.kind!=LockKind::Record) ||
               !names.emplace(node.kind,node.object_key).second || !nodes_.emplace(node.id,node).second)
                throw std::invalid_argument("duplicate/invalid DAG resource");
        }
        std::map<std::uint64_t,std::size_t> indegree;
        std::map<std::uint64_t,std::vector<std::uint64_t>> children;
        std::set<std::uint64_t> ready;
        for(const auto& item:nodes_) {
            const auto& n=item.second;std::set<std::uint64_t> unique;
            for(auto parent:n.parents) {
                auto p=nodes_.find(parent);
                if(parent==n.id || p==nodes_.end() || p->second.kind!=LockKind::Node || !unique.insert(parent).second)
                    throw std::invalid_argument("unknown/duplicate/invalid DAG parent");
                children[parent].push_back(n.id);
            }
            indegree[n.id]=n.parents.size();if(n.parents.empty())ready.insert(n.id);
        }
        if(ready.size()!=1)throw std::invalid_argument("DAG requires one connected root");
        root_=*ready.begin();
        if(nodes_.at(root_).kind!=LockKind::Node)throw std::invalid_argument("DAG root must be a container");
        while(!ready.empty()) {
            const auto id=*ready.begin();ready.erase(ready.begin());order_.push_back(id);
            unsigned depth=0;
            for(auto parent:nodes_.at(id).parents) {
                if(depth_.at(parent)==std::numeric_limits<unsigned>::max())throw std::invalid_argument("DAG depth overflow");
                depth=std::max(depth,depth_.at(parent)+1);
            }
            depth_[id]=depth;
            for(auto child:children[id])if(!--indegree[child])ready.insert(child);
        }
        if(order_.size()!=nodes_.size())throw std::invalid_argument("DAG contains cycle");
    }
    std::vector<HierarchyRequest> read_path(const std::vector<std::uint64_t>& path) const {
        if(path.empty() || path.front()!=root_)throw std::invalid_argument("DAG read path must start at root");
        std::vector<HierarchyRequest> plan;
        for(std::size_t i=0;i<path.size();++i) {
            auto found=nodes_.find(path[i]);if(found==nodes_.end())throw std::invalid_argument("unknown read path resource");
            const auto& n=found->second;
            if(i && std::find(n.parents.begin(),n.parents.end(),path[i-1])==n.parents.end())
                throw std::invalid_argument("read path is not an edge");
            plan.push_back({i+1==path.size()?LockMode::S:LockMode::IS,KeyRangeEnt::point(n.object_key),n.kind,depth_.at(n.id)});
        }
        return plan;
    }
    std::vector<HierarchyRequest> write_record(std::uint64_t id) const {
        const auto found=nodes_.find(id);
        if(found==nodes_.end() || found->second.kind!=LockKind::Record)throw std::invalid_argument("DAG write target must be canonical record");
        std::set<std::uint64_t> ancestry;std::vector<std::uint64_t> pending{id};
        while(!pending.empty()) {
            auto current=pending.back();pending.pop_back();
            if(!ancestry.insert(current).second)continue;
            for(auto parent:nodes_.at(current).parents)pending.push_back(parent);
        }
        std::vector<HierarchyRequest> plan;
        for(auto current:order_)if(ancestry.count(current)) {
            const auto& n=nodes_.at(current);
            plan.push_back({current==id?LockMode::X:LockMode::IX,KeyRangeEnt::point(n.object_key),n.kind,depth_.at(current)});
        }
        return plan;
    }
};
inline void check_dag_geometry(std::uint64_t width,std::size_t paths,std::size_t read_path=0) {
    if(!width || paths<2 || paths>32 || read_path>=paths)throw std::invalid_argument("DAG width/paths/read-path bounds");
}
inline std::uint64_t dag_bucket_id(std::uint64_t bucket,std::size_t index,std::size_t paths) {
    if(paths<2 || paths>32 || index>=paths)throw std::invalid_argument("DAG index bounds");
    const auto base=1+static_cast<std::uint64_t>(paths);
    if(bucket>(UINT64_MAX-base-index)/paths)throw std::invalid_argument("DAG resource ID overflow");
    return base+bucket*paths+index;
}
inline std::size_t dag_node_capacity(const WorkloadTrace& trace,std::uint64_t width,std::size_t paths) {
    check_dag_geometry(width,paths);
    idx_key_t max_key=trace.config().key_count?trace.config().key_count-1:0;
    for(const auto& txn:trace.transactions())for(const auto& op:txn.operations)
        max_key=std::max(max_key,op.kind==OperationKind::RangeRead && op.key<op.end_key?op.end_key-1:op.key);
    const auto last=dag_bucket_id(max_key/width,paths-1,paths);
    if(last>=SIZE_MAX)throw std::invalid_argument("DAG node capacity overflow");
    return static_cast<std::size_t>(last+1);
}
// Lazy regular identity-index topology, not a materialized graph of millions
// of records. Each shared record has one bucket parent in EVERY index path.
// Reads select one path; writes take all ancestors before canonical Record X.
inline std::vector<HierarchyRequest> dag_plan(const TransactionTrace& txn,std::uint64_t width,
                                             std::size_t paths,std::size_t read_path) {
    check_dag_geometry(width,paths,read_path);
    std::map<std::pair<unsigned,std::uint64_t>,HierarchyRequest> requests;
    auto add=[&](unsigned phase,std::uint64_t id,LockMode mode,LockKind kind) {
        auto key=std::make_pair(phase,id);
        auto [it,added]=requests.emplace(key,HierarchyRequest{mode,KeyRangeEnt::point(id),kind,phase});
        if(!added)it->second.mode=lock_mode_join(it->second.mode,mode);
    };
    for(const auto& op:txn.operations) {
        const bool scan=op.kind==OperationKind::RangeRead;
        const bool write=!scan && op.kind!=OperationKind::Read;
        if(scan && op.key>=op.end_key)continue;
        add(0,0,write?LockMode::IX:LockMode::IS,LockKind::Node);
        const auto first=op.key/width,last=(scan?op.end_key-1:op.key)/width;
        for(std::size_t index=0;index<paths;++index) {
            if(!write && index!=read_path)continue;
            add(1,index+1,write?LockMode::IX:LockMode::IS,LockKind::Node);
            for(auto bucket=first;;++bucket) {
                add(2,dag_bucket_id(bucket,index,paths),scan?LockMode::S:write?LockMode::IX:LockMode::IS,LockKind::Node);
                if(bucket==last)break;
            }
        }
        if(!scan)add(3,op.key,write?LockMode::X:LockMode::S,LockKind::Record);
    }
    std::vector<HierarchyRequest> plan;plan.reserve(requests.size());
    for(const auto& item:requests)plan.push_back(item.second);
    return plan;
}
}
