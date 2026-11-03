#include "index/rdma_btree.h"
#include "lock/remote_lock_manager.h"
#include "lock/wait_policy.h"
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
using namespace rrl;
std::string host;std::uint16_t data_port,lock_port,coord_port;std::uint64_t fingerprint;
void open(BTreeRdma& tree,RemoteLockManager& locks) {
    CHECK(tree.connect(host,data_port,1,fingerprint));
    locks.use_coordinator_transaction_ids();CHECK(locks.connect(host,lock_port));
}
void release(PolicyClient& client,RemoteLockManager& locks,txnid_t id) {
    CHECK(locks.unlock_all(id)==RCOK);CHECK(client.end(id).result==TxnResult::Ok);
}
void grant(PolicyClient& c,RemoteLockManager& l,txnid_t id,LockMode mode,KeyRangeEnt range,LockKind kind) {
    CHECK(c.acquire(id,mode,range,kind).result==TxnResult::Ok);CHECK(l.lock(id,mode,range,kind)==RCOK);
}
int main(int argc,char** argv){try {
    if(!std::getenv("RRL_TEST_DATA_PORT"))return 77;
    host=std::getenv("RRL_TEST_RDMA_HOST")?std::getenv("RRL_TEST_RDMA_HOST"):"127.0.0.1";
    data_port=std::stoul(std::getenv("RRL_TEST_DATA_PORT"));
    lock_port=std::stoul(std::getenv("RRL_TEST_RDMA_PORT"));
    coord_port=std::stoul(std::getenv("RRL_TEST_COORDINATOR_PORT"));
    RdmaConnection probe;CHECK(probe.connect_client(host,data_port));OffsetBTreeHeader h;
    CHECK(probe.read(probe.remote_memory().address,probe.remote_memory().rkey,&h,sizeof(h)));fingerprint=h.seed_fingerprint;
    BTreeRdma a,b;RemoteLockManager la,lb;open(a,la);open(b,lb);
    if(argc==4 && std::string(argv[1])=="--read") {
        a.prepare_thread(0,1024);void* value=nullptr;CHECK(a.lookup(std::stoull(argv[2]),value));
        CHECK(*static_cast<std::uint64_t*>(value)==std::stoull(argv[3]));a.finish_thread();CHECK(a.validate());
        std::cout<<"Independent-process committed value visible: OK\n";return 0;
    }
    if(argc==3 && std::string(argv[1])=="--increment") {
        const auto iterations=std::stoull(argv[2]);CHECK(iterations>0 && iterations<=100000);
        PolicyClient client(host,coord_port,15000);a.prepare_thread(0,1024);
        std::vector<IndexMutation> batch;batch.reserve(1);
        for(std::uint64_t n=0;n<iterations;++n) {
            std::uint64_t age=0;bool committed=false;
            for(unsigned attempt=0;attempt<10000 && !committed;++attempt) {
                auto txn=client.begin(WaitPolicy::WaitDie,age,10000000000ULL);CHECK(txn.result==TxnResult::Ok);age=txn.age;
                auto granted=client.acquire(txn.id,LockMode::X,KeyRangeEnt::point(777),LockKind::NextKey);
                if(granted.result!=TxnResult::Ok) {
                    CHECK(granted.result==TxnResult::Conflict);CHECK(client.end(txn.id).result==TxnResult::Ok);continue;
                }
                CHECK(la.lock(txn.id,LockMode::X,KeyRangeEnt::point(777),LockKind::NextKey)==RCOK);
                void* ptr=nullptr;CHECK(a.lookup(777,ptr));auto value=*static_cast<std::uint64_t*>(ptr)+1;
                CHECK(client.prepare_commit(txn.id).result==TxnResult::Ok);
                batch.clear();batch.push_back({777,&value,false});CHECK(a.apply_batch(batch)==RCOK);
                release(client,la,txn.id);committed=true;
            }
            CHECK(committed);
        }
        a.finish_thread();CHECK(a.validate());std::cout<<"Shared increment commits="<<iterations<<'\n';return 0;
    }
    if(argc==2 && (std::string(argv[1])=="--poison" || std::string(argv[1])=="--poison-active")) {
        if(std::string(argv[1])=="--poison-active") {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            std::uint64_t latch=0;
            do {CHECK(probe.read(probe.remote_memory().address,probe.remote_memory().rkey,&latch,sizeof(latch)));CHECK(std::chrono::steady_clock::now()<deadline);}while(!latch);
        }
        std::uint64_t one=1;CHECK(probe.write(probe.remote_memory().address+8,probe.remote_memory().rkey,&one,sizeof(one)));
        a.prepare_thread(0,32);bool failed=false;try{void* v;a.lookup(0,v);}catch(...){failed=true;}
        CHECK(failed && !a.healthy());a.finish_thread();
        std::cout<<"Poisoned data is rejected: OK\n";return 0;
    }
    if(argc==2 && std::string(argv[1])=="--hold-range") {
        PolicyClient ca(host,coord_port,15000);auto txn=ca.begin(WaitPolicy::NoWait,0,10000000000ULL);CHECK(txn.result==TxnResult::Ok);
        grant(ca,la,txn.id,LockMode::S,KeyRangeEnt::closed(900,1100),LockKind::NextKey);
        a.prepare_thread(0,1024);std::vector<KeyValue> rows;rows.reserve(1024);a.scan_range_into(KeyRangeEnt::closed(900,1100),rows);
        auto n=rows.size();std::cout<<"holding "<<n<<std::endl;char c;CHECK(bool(std::cin.get(c)));
        a.scan_range_into(KeyRangeEnt::closed(900,1100),rows);CHECK(rows.size()==n);a.finish_thread();
        release(ca,la,txn.id);std::cout<<"Cross-process phantom protection: OK\n";return 0;
    }
    if(argc==2 && std::string(argv[1])=="--try-insert") {
        PolicyClient cb(host,coord_port,15000);auto txn=cb.begin(WaitPolicy::NoWait,0,10000000000ULL);CHECK(txn.result==TxnResult::Ok);
        CHECK(cb.acquire(txn.id,LockMode::X,KeyRangeEnt::point(1000),LockKind::NextKey).result==TxnResult::Conflict);
        CHECK(cb.end(txn.id).result==TxnResult::Ok);std::cout<<"Cross-process insert blocked: OK\n";return 0;
    }
    for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
        PolicyClient ca(host,coord_port,15000),cb(host,coord_port,15000);
        auto older=ca.begin(policy,0,10000000000ULL),younger=cb.begin(policy,0,10000000000ULL);
        CHECK(older.result==TxnResult::Ok && younger.result==TxnResult::Ok && older.age<younger.age);
        grant(cb,lb,younger.id,LockMode::X,KeyRangeEnt::point(777),LockKind::NextKey);
        if(policy==WaitPolicy::NoWait) {
            CHECK(ca.acquire(older.id,LockMode::X,KeyRangeEnt::point(777),LockKind::NextKey).result==TxnResult::Conflict);
            release(cb,lb,younger.id);CHECK(ca.end(older.id).result==TxnResult::Ok);
        } else {
            std::promise<void> started;
            auto future=std::async(std::launch::async,[&] {
                started.set_value();grant(ca,la,older.id,LockMode::X,KeyRangeEnt::point(777),LockKind::NextKey);
                CHECK(ca.prepare_commit(older.id).result==TxnResult::Ok);
                a.prepare_thread(0,1024);std::uint64_t v=12345;
                std::vector<IndexMutation> batch{{777,&v,false}};CHECK(a.apply_batch(batch)==RCOK);a.finish_thread();
                release(ca,la,older.id);
            });
            started.get_future().wait();
            if(policy==WaitPolicy::WoundWait) {
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while(cb.check(younger.id).result!=TxnResult::Wounded)CHECK(std::chrono::steady_clock::now()<deadline);
                CHECK(cb.prepare_commit(younger.id).result==TxnResult::Wounded);
            } else CHECK(future.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
            // Neither the waiting nor wounded transaction has written data.
            release(cb,lb,younger.id);future.get();
            b.prepare_thread(0,1024);void* value=nullptr;CHECK(b.lookup(777,value));CHECK(*static_cast<std::uint64_t*>(value)==12345);b.finish_thread();
        }
        CHECK(la.get_lock_count()==0);CHECK(a.validate() && b.validate());
    }
    // Tombstone and reinsertion reuse shared storage; boundary values survive.
    PolicyClient ca(host,coord_port,15000);auto txn=ca.begin(WaitPolicy::NoWait,0,10000000000ULL);
    grant(ca,la,txn.id,LockMode::X,KeyRangeEnt::unbounded(),LockKind::Predicate);
    CHECK(ca.prepare_commit(txn.id).result==TxnResult::Ok);a.prepare_thread(0,1024);
    std::uint64_t value=42;std::vector<IndexMutation> changes{{UINT64_MAX,&value,false},{778,&value,false},{778,nullptr,true}};
    CHECK(a.apply_batch(changes)==RCOK);void* out=nullptr;CHECK(!a.lookup(778,out));CHECK(a.lookup(UINT64_MAX,out));CHECK(*static_cast<std::uint64_t*>(out)==42);
    a.finish_thread();release(ca,la,txn.id);CHECK(a.validate());CHECK(a.rdma_operations()>0 && a.read_bytes()>0 && a.write_bytes()>0);
    std::cout<<"Shared RDMA records, wait-die, wound-wait, strict release and boundaries: OK\n";return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
