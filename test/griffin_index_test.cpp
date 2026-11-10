#include "index/griffin.h"
#include <atomic>
#include <chrono>
#include <future>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

namespace rrl {
struct BTreeGriffinTestPeer {
    static void before_sync(BTreeGriffin&,std::function<void()>);
    static void after_hash_batch(BTreeGriffin&,std::function<void()>);
};
}
using namespace rrl;
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);}while(0)
struct SyncGate {
    std::promise<void> reached,release;
    std::shared_future<void> released=release.get_future().share();
    std::atomic<bool> first{true};
    void enter() {
        if(first.exchange(false)) {
            reached.set_value();
            if(released.wait_for(std::chrono::seconds(10))!=std::future_status::ready)
                throw std::runtime_error("test synchronization deadline");
        }
    }
};
int main(){try {
    std::uint64_t a=10,b=20,c=30;
    // Pause after the committed hash write, before ordered replication.
    BTreeGriffin tree(4);CHECK(tree.init(4)==RCOK);CHECK(tree.init(4)==ERROR);
    CHECK(tree.insert(1,&a)==RCOK);CHECK(tree.insert(99,&b)==RCOK);
    SyncGate gate;
    BTreeGriffinTestPeer::before_sync(tree,[&]{gate.enter();});
    tree.prepare_thread(0,16);
    CHECK(tree.apply_batch({{15,&c,false}})==RCOK);
    CHECK(gate.reached.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready);
    void* value=nullptr;CHECK(tree.lookup(15,value) && value==&c);
    bool blocked=false;
    try {tree.scan_range(KeyRangeEnt::half_open(10,20));}catch(const IndexSyncPending&){blocked=true;}
    CHECK(blocked);
    CHECK(tree.scan_range(KeyRangeEnt::half_open(20,30)).empty());
    CHECK(tree.scan_range(KeyRangeEnt::to_right(15)).size()==1); // open high boundary
    // Same key crosses committed batches; FIFO must not resurrect old keys.
    CHECK(tree.apply_batch({{15,nullptr,true},{15,&b,false},{99,nullptr,true}})==RCOK);
    gate.release.set_value();tree.wait_synced();
    CHECK(tree.validate());auto rows=tree.scan_range(KeyRangeEnt::unbounded());
    CHECK(rows.size()==2 && rows[1].key==15 && rows[1].value==&b);
    CHECK(!tree.lookup(99,value));
    CHECK(tree.apply_batch({{15,&c,false}})==RCOK);tree.wait_synced();
    CHECK(tree.lookup(15,value) && value==&c);
    CHECK(tree.apply_batch({{0,&c,false},{UINT64_MAX,&a,false}})==RCOK);tree.wait_synced();
    CHECK(tree.scan_range(KeyRangeEnt::point(UINT64_MAX)).size()==1);
    CHECK(tree.scan_range(KeyRangeEnt::open(0,0)).empty());
    CHECK(tree.validate());

    // Queue capacity is checked before any hash mutation.
    BTreeGriffin tiny(1);CHECK(tiny.init(4)==RCOK);
    CHECK(tiny.insert(99,&a)==RCOK);tiny.prepare_thread(0,1);
    SyncGate tiny_gate;BTreeGriffinTestPeer::before_sync(tiny,[&]{tiny_gate.enter();});
    CHECK(tiny.insert(1,&a)==RCOK);
    CHECK(tiny_gate.reached.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready);
    CHECK(tiny.insert(2,&b)==AbortCapacity);CHECK(!tiny.lookup(2,value));
    // Value-only and net-zero membership commits do not need queue space.
    CHECK(tiny.apply_batch({{99,&b,false}})==RCOK);
    CHECK(tiny.lookup(99,value) && value==&b);
    CHECK(tiny.erase(2)==RCOK);
    CHECK(tiny.apply_batch({{99,&c,false},{2,&c,false}})==AbortCapacity);
    CHECK(tiny.lookup(99,value) && value==&b && !tiny.lookup(2,value));
    CHECK(tiny.apply_batch({{99,nullptr,true},{99,&a,false}})==RCOK);
    CHECK(tiny.apply_batch({{2,&b,false},{2,nullptr,true}})==RCOK);
    CHECK(tiny.lookup(99,value) && value==&a && !tiny.lookup(2,value));
    const auto full=tiny.diagnostics();
    CHECK(full.queue_batches==1 && full.queue_peak_batches==1 && full.pending_keys==1);
    CHECK(full.committed_batches==5 && full.value_only_batches==4 && full.capacity_aborts==2);
    CHECK(full.membership_batches==1 && full.membership_operations==1);
    tiny_gate.release.set_value();tiny.wait_synced();CHECK(tiny.validate());
    const auto drained=tiny.diagnostics();
    CHECK(drained.queue_batches==0 && drained.pending_keys==0 && drained.synced_batches==1);
    CHECK(drained.tree_operations==1 && drained.sync_work.count==1 && drained.sync_lag.count==1);
    CHECK(drained.sync_lag.max_ns>=drained.sync_work.max_ns);
    tiny.reset_diagnostics();
    const auto cleared=tiny.diagnostics();
    CHECK(!cleared.committed_batches && !cleared.queue_peak_batches && !cleared.sync_lag.count);

    // Make exactly two sync groups; all queued cross-batch changes on 11
    // collapse to its final state. Pending still blocks throughout the delay.
    for(std::size_t limit:{1,8}) {
    BTreeGriffin grouped(8,limit);CHECK(grouped.init(4)==RCOK);grouped.prepare_thread(0,4);
    SyncGate group_gate;BTreeGriffinTestPeer::before_sync(grouped,[&]{group_gate.enter();});
    CHECK(grouped.insert(10,&a)==RCOK);
    CHECK(group_gate.reached.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready);
    CHECK(grouped.insert(11,&a)==RCOK);CHECK(grouped.erase(11)==RCOK);
    CHECK(grouped.insert(11,&c)==RCOK);
    CHECK(grouped.apply_batch({{12,&b,false},{12,nullptr,true}})==RCOK);
    CHECK(grouped.insert(14,&b)==RCOK);
    auto grouped_stats=grouped.diagnostics();
    CHECK(grouped_stats.queue_batches==5 && grouped_stats.pending_keys==3);
    blocked=false;
    try{grouped.scan_range(KeyRangeEnt::point(11));}catch(const IndexSyncPending&){blocked=true;}
    CHECK(blocked && grouped.scan_range(KeyRangeEnt::point(12)).empty());
    group_gate.release.set_value();grouped.wait_synced();CHECK(grouped.validate());
    CHECK(grouped.lookup(11,value) && value==&c && !grouped.lookup(12,value));
    grouped_stats=grouped.diagnostics();
    CHECK(grouped_stats.committed_batches==6 && grouped_stats.membership_batches==5);
    CHECK(grouped_stats.membership_operations==5 && grouped_stats.synced_batches==5);
    CHECK(grouped_stats.sync_cycles==(limit==1?5:2) && grouped_stats.tree_operations==(limit==1?5:3));
    CHECK(grouped_stats.sync_work.count==grouped_stats.sync_cycles && grouped_stats.sync_lag.count==5);
    CHECK(!grouped_stats.queue_batches && !grouped_stats.pending_keys);
    }


    // A replication fault retains the fence and is never a successful scan.
    BTreeGriffin broken;CHECK(broken.init(4)==RCOK);broken.prepare_thread(0,1);
    BTreeGriffinTestPeer::before_sync(broken,[]{throw std::runtime_error("injected replication fault");});
    CHECK(broken.insert(1,&a)==RCOK);
    bool failed=false;try{broken.wait_synced();}catch(const std::runtime_error&){failed=true;}
    CHECK(failed && !broken.healthy() && !broken.validate());

    // Failure after hash mutation must publish before blocked readers get
    // the shard latch; neither lookup nor scan may expose a partial batch.
    BTreeGriffin partial;CHECK(partial.init(4)==RCOK);partial.prepare_thread(0,1);
    SyncGate partial_gate;
    BTreeGriffinTestPeer::after_hash_batch(partial,[&]{
        partial_gate.enter();throw std::runtime_error("injected hash batch fault");
    });
    auto writer=std::async(std::launch::async,[&]{return partial.insert(1,&a);});
    CHECK(partial_gate.reached.get_future().wait_for(std::chrono::seconds(5))==std::future_status::ready);
    auto reader=std::async(std::launch::async,[&]{
        try {void* v=nullptr;partial.lookup(1,v);return false;}
        catch(const std::runtime_error&){return true;}
    });
    partial_gate.release.set_value();
    CHECK(writer.get()==ERROR && reader.get() && !partial.healthy());
    failed=false;
    try {partial.scan_range(KeyRangeEnt::unbounded());}catch(const std::runtime_error&){failed=true;}
    CHECK(failed && !partial.validate());

    // Concurrent unrelated membership changes and scans must preserve hash/tree.
    BTreeGriffin parallel;CHECK(parallel.init(8)==RCOK);parallel.prepare_thread(0,64);
    std::vector<std::uint64_t> values(400);for(std::size_t i=0;i<values.size();++i)values[i]=i;
    std::atomic<bool> error{false};std::vector<std::thread> workers;
    for(unsigned t=0;t<4;++t)workers.emplace_back([&,t]{
        for(unsigned n=t;n<values.size();n+=4) {
            if(parallel.insert(n,&values[n])!=RCOK)error=true;
            try {const auto r=parallel.scan_range(KeyRangeEnt::half_open(n,n+4));
                for(std::size_t i=1;i<r.size();++i)if(r[i-1].key>=r[i].key)error=true;
            }catch(const IndexSyncPending&){}catch(...){error=true;}
        }
    });
    for(auto& worker:workers)worker.join();
    parallel.wait_synced();
    CHECK(!error && parallel.size()==values.size() && parallel.validate());
    std::cout<<"Griffin hash/ordered index, committed pending fence, FIFO, capacity and failure: OK\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
