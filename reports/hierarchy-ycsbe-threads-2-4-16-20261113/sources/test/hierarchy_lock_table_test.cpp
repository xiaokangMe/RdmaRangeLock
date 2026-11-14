#include "lock/hierarchy_lock_table.h"
#include "txn/record_workload.h"
#include "../src/txn/compact_hierarchy_plan.h"
#include <future>
#include <iostream>
#include <random>
#include <thread>
using namespace rrl;
using Table=HierarchyLockTable;
static constexpr auto nw=WaitPolicy::NoWait;
static constexpr std::uint64_t budget=2000000000ULL;
static const LockMode modes[]={LockMode::IS,LockMode::IX,LockMode::S,LockMode::SIX,LockMode::X};
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
namespace rrl {
struct HierarchyLockTableTestAccess {
    static void publish(Table& t,std::size_t context,std::uint8_t code){t.lease(context,0).store(code,std::memory_order_release);}
};
}
void pairwise_and_upgrades() {
    Table table(2,3,nw);Table::Client a(table,0,2),b(table,1,2),c(table,2,1);
    for(auto m:modes)for(auto n:modes)for(auto target:modes) {
        a.begin(0,budget);b.begin(0,budget);c.begin(0,budget);
        CHECK(a.acquire<nw>(0,m).result==TxnResult::Ok);
        const auto result=b.acquire<nw>(0,n).result;
        CHECK((result==TxnResult::Ok)==!lock_modes_conflict(m,n));
        const auto joined=lock_mode_join(m,target);
        const bool allowed=result!=TxnResult::Ok || !lock_modes_conflict(joined,n);
        CHECK((a.acquire<nw>(0,target).result==TxnResult::Ok)==allowed);
        const auto final=allowed?joined:m;
        const auto before=table.locate(0)->load()&Table::payload_mask;
        CHECK(a.acquire<nw>(0,final).result==TxnResult::Ok);
        CHECK((table.locate(0)->load()&Table::payload_mask)==before);
        CHECK(a.acquire<nw>(1,LockMode::X).result==TxnResult::Ok);
        CHECK(c.acquire<nw>(0,LockMode::X).result==TxnResult::Conflict);
        // Releasing SIX must preserve another transaction's IS.
        a.finish();b.finish();c.finish();CHECK(table.empty());
    }
    a.begin(0,budget);CHECK(a.acquire<nw>(0,LockMode::U).result==TxnResult::Error);a.finish();
}
void all_contexts() {
    for(auto mode:{LockMode::IS,LockMode::IX,LockMode::S}) {
        Table table(1,64,nw);std::vector<std::unique_ptr<Table::Client>> clients;
        for(std::size_t c=0;c<64;++c) {
            clients.emplace_back(new Table::Client(table,c,1));clients.back()->begin(0,budget);
            CHECK(clients.back()->acquire<nw>(0,mode).result==TxnResult::Ok);
        }
        CHECK((table.locate(0)->load()&Table::payload_mask)==64*Table::contribution(mode,0));
        for(auto& client:clients)client->finish();
        CHECK(table.empty());
    }
    Table table(1,64,nw);std::vector<std::unique_ptr<Table::Client>> clients;
    for(std::size_t c=0;c<64;++c) {clients.emplace_back(new Table::Client(table,c,1));clients.back()->begin(0,budget);}
    CHECK(clients[63]->acquire<nw>(0,LockMode::SIX).result==TxnResult::Ok);
    for(std::size_t c=0;c<63;++c)CHECK(clients[c]->acquire<nw>(0,LockMode::IS).result==TxnResult::Ok);
    clients[63]->finish();CHECK((table.locate(0)->load()&Table::payload_mask)==63);
    for(auto& client:clients)client->finish();
    CHECK(table.empty());
    clients[63]->begin(0,budget);CHECK(clients[63]->acquire<nw>(0,LockMode::X).result==TxnResult::Ok);
    clients[63]->finish();CHECK(table.empty());
}
template<WaitPolicy P>void wait_and_generation() {
    Table table(2,3,P);Table::Client old(table,0,2),young(table,1,2),peer(table,2,2);
    for(unsigned iteration=0;iteration<100;++iteration) {
        old.begin(0,budget);young.begin(0,budget);peer.begin(0,budget);
        CHECK(old.template acquire<P>(0,LockMode::SIX).result==TxnResult::Ok);
        CHECK(peer.template acquire<P>(0,LockMode::IS).result==TxnResult::Ok);
        if constexpr(P==WaitPolicy::WaitDie)
            CHECK(young.template acquire<P>(0,LockMode::S).result==TxnResult::Conflict);
        else {
            auto waiting=std::async(std::launch::async,[&]{return young.template acquire<P>(0,LockMode::S);});
            old.finish();CHECK(waiting.get().result==TxnResult::Ok);
        }
        old.finish();young.finish();peer.finish();CHECK(table.empty());
        old.begin(0,budget);young.begin(0,budget);peer.begin(0,budget);
        CHECK(young.template acquire<P>(0,LockMode::IX).result==TxnResult::Ok);
        CHECK(peer.template acquire<P>(0,LockMode::IX).result==TxnResult::Ok);
        auto waiting=std::async(std::launch::async,[&]{return old.template acquire<P>(0,LockMode::S);});
        if constexpr(P==WaitPolicy::WoundWait) {
            const auto limit=Table::Clock::now()+std::chrono::seconds(1);
            while(young.check()!=TxnResult::Wounded || peer.check()!=TxnResult::Wounded)CHECK(Table::Clock::now()<limit);
        }
        young.finish();peer.finish();CHECK(waiting.get().result==TxnResult::Ok);old.finish();CHECK(table.empty());
    }
    old.begin(0,budget);young.begin(0,budget);
    CHECK(young.template acquire<P>(0,LockMode::X).result==TxnResult::Ok);
    CHECK(young.template prepare_commit<P>()==TxnResult::Ok);
    auto waiting=std::async(std::launch::async,[&]{return old.template acquire<P>(0,LockMode::S);});
    CHECK(young.check()==TxnResult::Ok);young.finish();CHECK(waiting.get().result==TxnResult::Ok);old.finish();
    CHECK(table.empty());
}
template<WaitPolicy P>void concurrent_publication() {
    Table table(8,8,P);std::vector<std::thread> threads;std::atomic<unsigned> errors{0};
    std::atomic<int> writers[8]{},readers[8]{};
    for(std::size_t c=0;c<8;++c)threads.emplace_back([&,c]{
        Table::Client client(table,c,2);std::mt19937_64 rng(c+19);
        for(unsigned i=0;i<2000;++i) {
            const auto node=rng()%8;const bool write=rng()%3==0;client.begin(0,budget);
            const auto reply=client.template acquire<P>(node,write?LockMode::X:LockMode::S);
            if(reply.result==TxnResult::Ok) {
                if(write) {
                    if(writers[node].fetch_add(1)!=0 || readers[node].load()!=0)++errors;
                    writers[node].fetch_sub(1);
                } else {
                    readers[node].fetch_add(1);if(writers[node].load()!=0)++errors;readers[node].fetch_sub(1);
                }
            } else if(reply.result!=TxnResult::Conflict && reply.result!=TxnResult::Wounded)++errors;
            client.finish();
        }
    });
    for(auto& t:threads)t.join();
    CHECK(errors.load()==0);CHECK(table.empty());
}
void publication_window() {
    constexpr auto P=WaitPolicy::WoundWait;
    Table table(1,2,P);Table::Client old(table,0,1),young(table,1,1);
    for(unsigned i=0;i<100;++i) {
        old.begin(0,budget);young.begin(0,budget);
        CHECK(young.acquire<P>(0,LockMode::S).result==TxnResult::Ok);
        // Force the exact post-authority / pre-stable-lease window. The
        // requester must resample; a reservation is not a holder identity.
        HierarchyLockTableTestAccess::publish(table,1,128);
        std::atomic<bool> started{false};
        auto future=std::async(std::launch::async,[&]{started=true;return old.acquire<P>(0,LockMode::X);});
        while(!started.load())std::this_thread::yield();
        CHECK(future.wait_for(std::chrono::milliseconds(1))==std::future_status::timeout);
        CHECK(young.check()==TxnResult::Ok);
        HierarchyLockTableTestAccess::publish(table,1,static_cast<std::uint8_t>(LockMode::S)+1);
        const auto limit=Table::Clock::now()+std::chrono::seconds(1);
        while(young.check()!=TxnResult::Wounded)CHECK(Table::Clock::now()<limit);
        young.finish();CHECK(future.get().result==TxnResult::Ok);old.finish();CHECK(table.empty());
        // Reuse that same slot for a new, younger generation. Old wounds
        // must not survive begin or be interpreted as a new grant.
        young.begin(0,budget);CHECK(young.check()==TxnResult::Ok);young.finish();
    }
}
void executor_and_phantom() {
    for(auto width:{1ULL,16ULL,256ULL}) {
        WorkloadConfig config;config.key_count=32;config.transaction_count=1;config.threads=1;
        config.range_length=32;config.hierarchy_bucket_width=width;
        const std::vector<Operation> ops={{OperationKind::RangeRead,0,32,0},
            {OperationKind::Insert,15,0,150},{OperationKind::RangeRead,10,20,0},
            {OperationKind::Delete,10,0,0},{OperationKind::Read,10,0,0},
            {OperationKind::Insert,10,0,100},{OperationKind::Read,15,0,0},
            {OperationKind::Insert,UINT64_MAX,0,9},{OperationKind::Read,UINT64_MAX,0,0}};
        const auto trace=WorkloadTrace::from_transactions(config,{{1,ops}});
        for(auto policy:{nw,WaitPolicy::WaitDie,WaitPolicy::WoundWait})for(bool implicit:{false,true}) {
            BTreeLatch a,b;a.init(4);b.init(4);LockManager generic;
            PolicyRunConfig cfg;cfg.protocol=PolicyProtocol::Hierarchy;cfg.wait_policy=policy;
            cfg.use_coroutines=false;cfg.hierarchy_implicit_root=implicit;
            const auto expected=run_policy_workload(a,generic,trace,cfg);
            const auto actual=run_compact_hierarchy_workload(b,trace,cfg);
            CHECK(!expected.failed() && !actual.failed());CHECK(expected.committed==1 && actual.committed==1);
            CHECK(expected.checksum==actual.checksum && expected.rows_found==actual.rows_found);
            CHECK(a.validate() && b.validate() && a.size()==b.size());
        }
    }
    WorkloadConfig config;config.key_count=32;config.threads=2;config.hierarchy_bucket_width=16;
    auto trace=WorkloadTrace::from_transactions(config,{
        {1,{{OperationKind::RangeRead,12,15,0}}},{2,{{OperationKind::Insert,13,0,1}}},
        {3,{{OperationKind::Insert,30,0,1}}}});
    detail::CompactHierarchyPlans plans(trace,false);Table table(plans.nodes,3,nw);
    Table::Client scan(table,0,8),inside(table,1,8),outside(table,2,8);
    auto take=[&](auto& client,std::size_t index){
        for(const auto& r:plans.plans[index]) {
            const auto result=client.template acquire_planned<nw>(r.node,r.mode).result;
            if(result!=TxnResult::Ok)return result;
        }return TxnResult::Ok;
    };
    scan.begin(0,budget);inside.begin(0,budget);outside.begin(0,budget);
    CHECK(take(scan,0)==TxnResult::Ok);
    auto future=std::async(std::launch::async,[&]{return take(inside,1);});
    CHECK(future.get()==TxnResult::Conflict);inside.finish();
    CHECK(take(outside,2)==TxnResult::Ok);outside.finish();scan.finish();
    inside.begin(0,budget);CHECK(take(inside,1)==TxnResult::Ok);inside.finish();CHECK(table.empty());
}
void limits() {
    Table table(2,1,nw);Table::Client client(table,0,1);client.begin(0,budget);
    CHECK(client.acquire<nw>(0,LockMode::S).result==TxnResult::Ok);
    CHECK(client.acquire<nw>(1,LockMode::S).result==TxnResult::Conflict && client.capacity_abort());
    client.finish();CHECK(table.empty());
    table.locate(0)->store(Table::sequence_limit);client.begin(0,budget);
    CHECK(client.acquire<nw>(0,LockMode::S).result==TxnResult::Error);client.finish();
    table.locate(0)->store(0);CHECK(table.empty());
}
int main(){try {
    pairwise_and_upgrades();all_contexts();limits();
    publication_window();executor_and_phantom();
    wait_and_generation<WaitPolicy::WaitDie>();wait_and_generation<WaitPolicy::WoundWait>();
    concurrent_publication<nw>();concurrent_publication<WaitPolicy::WaitDie>();concurrent_publication<WaitPolicy::WoundWait>();
    std::cout<<"Five-mode CAS: compatibility, joins, 64 contexts, wait/wound, generation reuse, publication, limits OK\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
