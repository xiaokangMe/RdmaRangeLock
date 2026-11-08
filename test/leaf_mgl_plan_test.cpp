#include "index/btree.h"
#include "../src/txn/leaf_hierarchy.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <vector>

using namespace rrl;

static void require(bool value,const char* message) {
    if(!value) throw std::runtime_error(message);
}
static bool phase_ordered(const std::vector<detail::HierarchyRequest>& plan) {
    return std::is_sorted(plan.begin(),plan.end(),[](const auto& lhs,const auto& rhs) {
        return lhs.phase < rhs.phase;
    });
}
static const detail::HierarchyRequest* find_node(const std::vector<detail::HierarchyRequest>& plan,
                                                  std::uint64_t id) {
    const auto it=std::find_if(plan.begin(),plan.end(),[&](const auto& request) {
        return request.kind==LockKind::Node && request.range.StartKey==id;
    });
    return it==plan.end()?nullptr:&*it;
}
static const detail::HierarchyRequest* find_record(const std::vector<detail::HierarchyRequest>& plan,
                                                    idx_key_t key) {
    const auto it=std::find_if(plan.begin(),plan.end(),[&](const auto& request) {
        return request.kind==LockKind::Record && request.range.StartKey==key;
    });
    return it==plan.end()?nullptr:&*it;
}

int main() {
    try {
        BTreeLatch tree;
        require(tree.init(4)==RCOK,"tree init");
        std::vector<std::uint64_t> values(128);
        for(idx_key_t key=0;key<32;++key) {
            values[key]=key;
            require(tree.insert(key,&values[key])==RCOK,"tree seed");
        }

        std::vector<IndexNodeSpan> scratch;
        scratch.reserve(512);
        std::vector<detail::HierarchyRequest> plan;
        plan.reserve(1024);

        TransactionTrace scan;
        scan.operations={{OperationKind::RangeRead,5,23,0}};
        require(detail::leaf_plan(tree,scan,scratch,plan),"range plan");
        require(phase_ordered(plan),"range plan parent-before-child");
        const auto* root=find_node(plan,0);
        require(root && root->mode==LockMode::IS,"range root IS");
        require(std::any_of(plan.begin(),plan.end(),[](const auto& request) {
            return request.kind==LockKind::Node && request.mode==LockMode::S && request.phase>0;
        }),"range locks real leaf S");
        require(!find_record(plan,5) && !std::any_of(plan.begin(),plan.end(),[](const auto& request) {
            return request.phase==std::numeric_limits<unsigned>::max();
        }),"range has no record phase");

        TransactionTrace read;
        read.operations={{OperationKind::Read,7,0,0}};
        require(detail::leaf_plan(tree,read,scratch,plan),"point read plan");
        require(phase_ordered(plan) && find_node(plan,0) && find_node(plan,0)->mode==LockMode::IS,"point read root IS");
        require(find_record(plan,7) && find_record(plan,7)->mode==LockMode::S &&
                find_record(plan,7)->phase==std::numeric_limits<unsigned>::max(),"point read record S last");

        TransactionTrace write;
        write.operations={{OperationKind::Update,7,0,99}};
        require(detail::leaf_plan(tree,write,scratch,plan),"point write plan");
        require(phase_ordered(plan) && find_node(plan,0) && find_node(plan,0)->mode==LockMode::IX,"point write root IX");
        require(find_record(plan,7) && find_record(plan,7)->mode==LockMode::X &&
                find_record(plan,7)->phase==std::numeric_limits<unsigned>::max(),"point write record X last");
        require(std::any_of(plan.begin(),plan.end(),[](const auto& request) {
            return request.kind==LockKind::Node && request.mode==LockMode::IX && request.phase>0;
        }),"point write leaf IX");

        TransactionTrace mixed;
        mixed.operations={{OperationKind::RangeRead,5,23,0},{OperationKind::Update,7,0,99}};
        require(detail::leaf_plan(tree,mixed,scratch,plan),"mixed plan");
        require(phase_ordered(plan) && std::any_of(plan.begin(),plan.end(),[](const auto& request) {
            return request.kind==LockKind::Node && request.mode==LockMode::SIX;
        }),"mixed leaf SIX");

        // A full leaf split must be fenced by an existing real ancestor.
        BTreeLatch split;
        require(split.init(3)==RCOK,"split init");
        for(idx_key_t key=0;key<3;++key) {
            values[key]=key;
            require(split.insert(key,&values[key])==RCOK,"split seed");
        }
        TransactionTrace insert;
        insert.operations={{OperationKind::Insert,3,0,33}};
        require(detail::leaf_plan(split,insert,scratch,plan),"split insert plan");
        require(phase_ordered(plan) && find_node(plan,0) && find_node(plan,0)->mode==LockMode::IX,
                "split insert root intent");
        require(std::any_of(plan.begin(),plan.end(),[](const auto& request) {
            return request.kind==LockKind::Node && request.mode==LockMode::X && request.range.StartKey!=0;
        }),"split insert real node X");
        const auto stale=plan;
        require(split.insert(3,&values[3])==RCOK,"split publish");
        TransactionTrace after_split;
        after_split.operations={{OperationKind::Read,3,0,0}};
        require(detail::leaf_plan(split,after_split,scratch,plan),"post-split plan");
        require(!detail::covers_hierarchy_plan(stale,plan),"post-split path requires revalidation");

        // Deleting the only key in a child can collapse the root. The
        // synthetic root X prevents a replacement root from escaping.
        BTreeLatch collapse;
        require(collapse.init(3)==RCOK,"collapse init");
        for(idx_key_t key=0;key<3;++key) {
            values[key]=key;
            require(collapse.insert(key,&values[key])==RCOK,"collapse seed");
        }
        TransactionTrace erase;
        erase.operations={{OperationKind::Delete,0,0,0}};
        require(detail::leaf_plan(collapse,erase,scratch,plan),"collapse plan");
        require(find_node(plan,0) && find_node(plan,0)->mode==LockMode::X,"collapse root X");
        const auto held=plan;
        require(collapse.erase(0)==RCOK,"collapse publish");
        TransactionTrace after_erase;
        after_erase.operations={{OperationKind::Read,1,0,0}};
        require(detail::leaf_plan(collapse,after_erase,scratch,plan),"post-collapse plan");
        require(!detail::covers_hierarchy_plan(held,plan),"post-collapse path requires revalidation");
        require(tree.validate() && split.validate() && collapse.validate(),"tree invariants");

        std::cout<<"real B+tree leaf MGL plan, modes, split and root-collapse fences: OK\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
