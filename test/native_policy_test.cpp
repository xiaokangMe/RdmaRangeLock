#include "lock/native_policy.h"
#include "lock/remote_lock_manager.h"
#include "txn/coroutine.h"
#include "common/execution_context.h"
#include "../src/lock/native_txn_layout.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
using namespace rrl;
using namespace rrl::native_detail;
namespace {
void require(bool b,const char* what){if(!b)throw std::runtime_error(what);}
void ok(PolicyReply r){require(r.result==TxnResult::Ok,"expected transaction OK");}
}
struct PrefixFailure final : LockManager {
    unsigned calls{0};
    RC lock(txnid_t t,LockMode m,const KeyRangeEnt& r,LockKind k,std::vector<txnid_t>* c) override {
        if(++calls==2)return AbortCapacity;
        return LockManager::lock(t,m,r,k,c);
    }
};
int main(int argc,char** argv) {
 try {
    const auto* hostenv=std::getenv("RRL_TEST_RDMA_HOST");
    const auto* portenv=std::getenv("RRL_TEST_RDMA_PORT");
    const auto* stateenv=std::getenv("RRL_TEST_TXN_STATE_PORT");
    if(!portenv || !stateenv)return 77;
    const std::string host=hostenv?hostenv:"127.0.0.1";
    const auto port=static_cast<std::uint16_t>(std::stoul(portenv));
    const auto stateport=static_cast<std::uint16_t>(std::stoul(stateenv));
    const auto policy=argc>1?parse_wait_policy(argv[1]):WaitPolicy::WoundWait;
    const auto range=KeyRangeEnt::point(98765);
    RemoteLockManager locks;locks.use_coordinator_transaction_ids();locks.set_connection_count(4);
    require(locks.connect(host,port),"physical lock connect");
    NativePolicyClient old(host,stateport,locks),young(host,stateport,locks);
    auto older=old.begin(policy,0,10000000000ULL);
    auto younger=young.begin(policy,0,10000000000ULL);
    ok(older);ok(younger);require(older.age<younger.age,"globally ordered age");
    ok(young.acquire(younger.id,LockMode::X,range,LockKind::Record));
    bool granted=false,holder_released=false,wound_seen=false;
    if(policy==WaitPolicy::NoWait) {
        require(old.acquire(older.id,LockMode::X,range,LockKind::Record).result==TxnResult::Conflict,"no-wait must abort");
        require(locks.get_lock_count(younger.id)==1,"conflict cannot steal holder lock");
        ok(young.prepare_commit(younger.id));
        require(locks.unlock_all(younger.id)==RCOK,"holder release");ok(young.end(younger.id));
        ok(old.acquire(older.id,LockMode::X,range,LockKind::Record));
    } else {
        CoroutineScheduler scheduler(2,262144);
        scheduler.run([&](std::size_t c) {
            if(c==0) {
                ok(old.acquire(older.id,LockMode::X,range,LockKind::Record));
                granted=true;require(holder_released,"grant before actual holder cleanup");
            } else {
                for(int i=0;i<100;++i) {
                    const auto status=young.check(younger.id).result;
                    if(status==TxnResult::Wounded){wound_seen=true;break;}
                    require(status==TxnResult::Ok,"holder state unexpectedly invalid");
                    require(!granted,"waiter acquired early");cooperative_yield();
                }
                if(policy==WaitPolicy::WoundWait)require(wound_seen,"older must wound younger");
                else require(!wound_seen,"wait-die must not wound");
                require(locks.get_lock_count(younger.id)==1,"wound must retain actual locks");
                require(locks.unlock_all(younger.id)==RCOK,"physical holder cleanup");
                holder_released=true;ok(young.end(younger.id));
            }
        });
        require(granted,"older must make progress");
    }
    require(locks.unlock_all(older.id)==RCOK,"older release");ok(old.end(older.id));
    // A younger Wait-Die requester must die, and retries retain original age.
    older=old.begin(policy,older.age,10000000000ULL);ok(older);
    ok(old.acquire(older.id,LockMode::X,range,LockKind::Record));
    younger=young.begin(policy,younger.age,10000000000ULL);ok(younger);
    if(policy!=WaitPolicy::WoundWait)
        require(young.acquire(younger.id,LockMode::X,range,LockKind::Record).result==TxnResult::Conflict,"younger should die");
    require(locks.unlock_all(older.id)==RCOK,"older release2");ok(old.end(older.id));ok(young.end(younger.id));

    RdmaConnection raw;require(raw.connect_client(host,stateport),"raw state probe");
    const auto m=raw.remote_memory();Header header;
    require(raw.read(m.address,m.rkey,&header,sizeof(header)),"state header");
    // Late wound cannot hit a reused worker/coroutine slot.
    auto a=young.begin(policy,0,10000000000ULL);ok(a);ok(young.end(a.id));
    auto b=young.begin(policy,0,10000000000ULL);ok(b);
    require(a.id%header.slots==b.id%header.slots && a.id!=b.id,"generation reuse");
    auto addr=m.address+sizeof(Header)+(b.id%header.slots)*sizeof(Slot);
    std::uint64_t prior=0;
    require(raw.compare_swap(addr,m.rkey,control(a.id,Running),control(a.id,Wounded),prior),"stale wound CAS transport");
    require(prior==control(b.id,Running),"late wound corrupted generation");ok(young.check(b.id));
    ok(young.end(b.id));
    // True concurrent NIC CAS competition: one winner, never both. No CPU/NIC
    // mixed atomics. Probe implements exactly the wound state transition.
    for(int round=0;round<100;++round) {
        b=young.begin(policy,0,10000000000ULL);ok(b);
        addr=m.address+sizeof(Header)+(b.id%header.slots)*sizeof(Slot);
        std::atomic<bool> go{false};
        std::uint64_t observed=0;bool transported=false;
        std::thread wound([&]{
            while(!go.load())std::this_thread::yield();
            transported=raw.compare_swap(addr,m.rkey,control(b.id,Running),control(b.id,Wounded),observed);
        });
        go=true;
        const auto prepared=young.prepare_commit(b.id);wound.join();
        require(transported,"wound transport");
        const bool won=observed==control(b.id,Running);
        require((won && prepared.result==TxnResult::Wounded) ||
                (!won && prepared.result==TxnResult::Ok && observed==control(b.id,Committing)),"wound/commit atomic fence");
        ok(young.end(b.id));
    }
    // Committing holder cannot be wounded by the real acquisition policy.
    older=old.begin(policy,0,10000000000ULL);ok(older);
    younger=young.begin(policy,0,10000000000ULL);ok(younger);
    ok(young.acquire(younger.id,LockMode::X,range,LockKind::Record));
    ok(young.prepare_commit(younger.id));granted=false;holder_released=false;
    CoroutineScheduler commit(2,262144);
    commit.run([&](std::size_t c) {
        if(c==0) {
            auto r=old.acquire(older.id,LockMode::X,range,LockKind::Record);
            if(policy==WaitPolicy::NoWait)require(r.result==TxnResult::Conflict,"no-wait committing holder");
            else {ok(r);require(holder_released,"committing holder stolen");granted=true;}
        } else {
            for(int i=0;i<50;++i){ok(young.check(younger.id));cooperative_yield();}
            require(locks.unlock_all(younger.id)==RCOK,"committing release");
            holder_released=true;ok(young.end(younger.id));
        }
    });
    if(policy!=WaitPolicy::NoWait)require(locks.unlock_all(older.id)==RCOK,"final release");
    ok(old.end(older.id));
    require(locks.get_lock_count()==0,"physical lock leak");
    // Non-batch physical backends must retain a successful prefix even
    // when a later request hits capacity.
    PrefixFailure prefix;
    NativePolicyClient prefix_client(host,stateport,prefix);
    const auto prefix_tx=prefix_client.begin(policy,0,1000000000ULL);ok(prefix_tx);
    require(prefix_client.acquire_batch(prefix_tx.id,LockMode::S,
        {KeyRangeEnt::point(1),KeyRangeEnt::point(2)},LockKind::Record).result==TxnResult::Conflict,"batch prefix abort");
    require(prefix_client.has_physical_locks() && prefix.get_lock_count(prefix_tx.id)==1,"native prefix tracked");
    require(prefix.unlock_all(prefix_tx.id)==RCOK,"native prefix cleanup");ok(prefix_client.end(prefix_tx.id));
    // Unannounced active disconnect poisons the passive cohort via NIC.
    if(argc==1 || argc>2) {
    const auto child=fork();require(child>=0,"fork");
    if(child==0) {
        try {
            NativePolicyClient abandoned(host,stateport,locks);
            const auto attempt=abandoned.begin(policy,0,1000000000ULL);
            _exit(attempt.result==TxnResult::Ok?0:4);
        }catch(...){_exit(5);}
    }
    int exit=0;waitpid(child,&exit,0);require(WIFEXITED(exit)&&WEXITSTATUS(exit)==0,"crash child");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    std::uint64_t poison=0;
    do {
        require(raw.read(m.address+offsetof(Header,poison),m.rkey,&poison,8),"read poison");
        if(poison)break;
        std::this_thread::yield();
    }while(std::chrono::steady_clock::now()<deadline);
    require(poison!=0,"active disconnect must poison");
    require(old.begin(policy,0,1000000000ULL).result==TxnResult::Error,"poison must block new txn");
    }
    std::cout<<"Native "<<wait_policy_name(policy)<<": physical authority, strict release, age, generation, 100 commit races and optional disconnect poison OK\n";
    return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
