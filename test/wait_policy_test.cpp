#include "lock/wait_policy.h"
#include "txn/workload.h"
#include <atomic>
#include <future>
#include <thread>
#include <iostream>
#include <cstdlib>
using namespace rrl;
namespace {
void check(bool v,const char* msg){if(!v){std::cerr<<"FAIL: "<<msg<<std::endl;std::exit(1);}}
void queued(TransactionCoordinator& e) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!e.waiting() && std::chrono::steady_clock::now()<end)std::this_thread::yield();
    check(e.waiting()>0,"request entered condition-variable wait set");
}
void ok(PolicyReply r,const char* m){check(r.result==TxnResult::Ok,m);}
}
int main() {
    const auto x=KeyRangeEnt::point(7),y=KeyRangeEnt::point(8);
    constexpr std::uint64_t budget=2000000000ULL;
    for(auto p:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
        TransactionCoordinator e;PolicyClient old(e,1),young(e,2);
        const auto a=old.begin(p,0,budget),b=young.begin(p,0,budget);
        ok(a,"older begin");ok(b,"younger begin");
        ok(old.acquire(a.id,LockMode::X,x,LockKind::Record),"older owns X");
        if(p!=WaitPolicy::WoundWait) {
            check(young.acquire(b.id,LockMode::S,x,LockKind::Record).result==TxnResult::Conflict,"younger dies under no-wait/wait-die");
            ok(old.end(a.id),"older end");
        } else {
            auto f=std::async(std::launch::async,[&]{return young.acquire(b.id,LockMode::S,x,LockKind::Record);});
            queued(e);ok(old.prepare_commit(a.id),"younger does not wound older");ok(old.end(a.id),"release wakes younger");
            auto r=f.get();ok(r,"younger wound-wait wakes");check(r.wait_ns>0,"positive lock wait");
        }
        ok(young.end(b.id),"younger end");check(e.active()==0,"no metadata leaks");
    }
    for(auto p:{WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
        TransactionCoordinator e;PolicyClient old(e,1),young(e,2);
        auto a=old.begin(p,0,budget),b=young.begin(p,0,budget);
        ok(young.acquire(b.id,LockMode::X,x,LockKind::Record),"young owns X");
        auto f=std::async(std::launch::async,[&]{return old.acquire(a.id,LockMode::S,x,LockKind::Record);});
        queued(e);
        const auto r=young.prepare_commit(b.id);
        check(r.result==(p==WaitPolicy::WoundWait?TxnResult::Wounded:TxnResult::Ok),"wound is atomic with commit permission");
        ok(young.end(b.id),"young releases");ok(f.get(),"older wakes");ok(old.end(a.id),"old cleanup");
        auto retry=young.begin(p,b.age,budget);
        check(retry.id>b.id && retry.age==b.age,"attempt identity changes but timestamp survives restart");
        ok(young.end(retry.id),"retry cleanup");
    }
    {
        TransactionCoordinator e;PolicyClient old(e,1),young(e,2);
        auto a=old.begin(WaitPolicy::WoundWait,0,budget),b=young.begin(WaitPolicy::WoundWait,0,budget);
        ok(young.acquire(b.id,LockMode::X,x,LockKind::Record),"commit fence setup");
        ok(young.prepare_commit(b.id),"young wins commit fence");
        auto f=std::async(std::launch::async,[&]{return old.acquire(a.id,LockMode::X,x,LockKind::Record);});
        queued(e);ok(young.check(b.id),"committing victim cannot be wounded");ok(young.end(b.id),"committed release");
        ok(f.get(),"older proceeds after committed transaction");ok(old.end(a.id),"cleanup");
    }
    // Opposite lock order: wait-die aborts the young edge; wound-wait wakes a
    // young waiter and aborts it, rather than leaving both transactions asleep.
    for(auto p:{WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
        TransactionCoordinator e;PolicyClient old(e,1),young(e,2);
        auto a=old.begin(p,0,budget),b=young.begin(p,0,budget);
        ok(old.acquire(a.id,LockMode::X,x,LockKind::Record),"old x");
        ok(young.acquire(b.id,LockMode::X,y,LockKind::Record),"young y");
        if(p==WaitPolicy::WaitDie) {
            auto f=std::async(std::launch::async,[&]{return old.acquire(a.id,LockMode::X,y,LockKind::Record);});
            queued(e);check(young.acquire(b.id,LockMode::X,x,LockKind::Record).result==TxnResult::Conflict,"wait-die removes young cycle edge");
            ok(young.end(b.id),"young abort release");ok(f.get(),"old no deadlock");
        } else {
            auto f=std::async(std::launch::async,[&]{return young.acquire(b.id,LockMode::X,x,LockKind::Record);});
            queued(e);
            auto g=std::async(std::launch::async,[&]{return old.acquire(a.id,LockMode::X,y,LockKind::Record);});
            check(f.get().result==TxnResult::Wounded,"wounded waiting transaction is cancelled");
            ok(young.end(b.id),"wounded abort release");ok(g.get(),"wound-wait no deadlock");
        }
        ok(old.end(a.id),"old cleanup");
    }
    {
        TransactionCoordinator e;PolicyClient a(e,1),b(e,2);
        auto x1=a.begin(WaitPolicy::WoundWait,0,budget),x2=b.begin(WaitPolicy::WoundWait,0,2000000);
        ok(a.acquire(x1.id,LockMode::X,x,LockKind::Record),"timeout holder");
        check(b.acquire(x2.id,LockMode::X,x,LockKind::Record).result==TxnResult::Deadline,"deadline cancels blocking call");
        ok(b.end(x2.id),"timed-out cleanup");ok(a.end(x1.id),"holder cleanup");
    }
    {
        TransactionCoordinator e;PolicyClient a(e,1),b(e,2);
        auto x1=a.begin(WaitPolicy::WoundWait,0,budget),x2=b.begin(WaitPolicy::WoundWait,0,budget);
        ok(a.acquire(x1.id,LockMode::X,x,LockKind::Record),"disconnect holder");
        auto f=std::async(std::launch::async,[&]{return b.acquire(x2.id,LockMode::X,x,LockKind::Record);});
        queued(e);e.disconnect(1);
        check(f.get().result==TxnResult::Error,"disconnect poisons and wakes all waiters");
        check(a.prepare_commit(x1.id).result==TxnResult::Error,"poison prohibits commit");
        ok(a.end(x1.id),"explicit known cleanup");ok(b.end(x2.id),"waiter cleanup");
    }
    // Competing S -> X conversions must break the same age cycle as fresh
    // requests. A successful conversion may not bypass the other holder.
    for(auto p:{WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
        TransactionCoordinator e;PolicyClient old(e,1),young(e,2);
        const auto a=old.begin(p,0,budget),b=young.begin(p,0,budget);
        ok(old.acquire(a.id,LockMode::S,x,LockKind::Record),"old shared upgrade setup");
        ok(young.acquire(b.id,LockMode::S,x,LockKind::Record),"young shared upgrade setup");
        auto f=std::async(std::launch::async,[&]{return old.acquire(a.id,LockMode::X,x,LockKind::Record);});
        queued(e);
        check(young.acquire(b.id,LockMode::X,x,LockKind::Record).result==
              (p==WaitPolicy::WaitDie?TxnResult::Conflict:TxnResult::Wounded),"young conversion aborts");
        ok(young.end(b.id),"young conversion releases");ok(f.get(),"old conversion wakes");
        ok(old.prepare_commit(a.id),"old conversion commits");
        check(old.acquire(a.id,LockMode::S,y,LockKind::Record).result==TxnResult::Error,"commit fence forbids new locks");
        ok(old.end(a.id),"upgrade cleanup");check(e.active()==0,"upgrade leaves no locks");
    }
    {
        TransactionCoordinator e;PolicyClient a(e,1),b(e,2);
        const auto t=a.begin(WaitPolicy::WaitDie,0,budget);
        check(b.begin(WaitPolicy::WoundWait,0,budget).result==TxnResult::Error,"active cohort cannot mix age policies");
        check(b.acquire(t.id,LockMode::X,x,LockKind::Record).result==TxnResult::Error,"session cannot use another attempt ID");
        check(b.end(t.id).result==TxnResult::Error,"session cannot release another attempt");
        ok(a.end(t.id),"session cleanup");
        check(b.begin(WaitPolicy::WaitDie,t.age,budget).result==TxnResult::Error,"session cannot inherit foreign age");
        const auto next=b.begin(WaitPolicy::WoundWait,0,budget);ok(next,"empty cohort may change policy");
        ok(b.end(next.id),"new cohort cleanup");
    }
    // Predicate protects an empty result interval, with exact half-open bounds.
    {
        TransactionCoordinator e;PolicyClient a(e,1),b(e,2);
        auto x1=a.begin(WaitPolicy::NoWait,0,budget),x2=b.begin(WaitPolicy::NoWait,0,budget);
        ok(a.acquire(x1.id,LockMode::S,KeyRangeEnt::half_open(10,20),LockKind::Predicate),"empty predicate S");
        check(b.acquire(x2.id,LockMode::X,KeyRangeEnt::point(15),LockKind::Predicate).result==TxnResult::Conflict,"predicate blocks phantom insert");
        ok(b.acquire(x2.id,LockMode::X,KeyRangeEnt::point(20),LockKind::Predicate),"exclusive upper bound writable");
        ok(b.end(x2.id),"predicate writer cleanup");ok(a.end(x1.id),"predicate reader cleanup");
    }
    {
        // Bucket limits must include the true sample without exceeding max,
        // including UINT64_MAX's bucket. Each distribution has one sample.
        for(const auto sample:{0ULL,63ULL,64ULL,127ULL,128ULL,255ULL,256ULL,1023ULL,1024ULL,
                               9223372036854775808ULL,18446744073709551614ULL,18446744073709551615ULL}) {
            LatencyHistogram edge;edge.add(sample);const auto q=edge.summary();
            check(q.count==1 && q.p50_ns==sample && q.p999_ns==sample && q.max_ns==sample,"histogram single sample at bucket boundary");
        }
    }
    LatencyHistogram h;
    for(unsigned i=1;i<=10000;++i)h.add(i);
    auto q=h.summary();check(q.count==10000 && q.p50_ns>=5000 && q.p50_ns<5080 && q.p99_ns>=9900 && q.p99_ns<=10000,"histogram quantiles match known distribution");
    {
        TransactionCoordinator e;PolicyClient a(e,1),b(e,2);
        auto x1=a.begin(WaitPolicy::NoWait,0,budget),x2=b.begin(WaitPolicy::NoWait,0,budget);
        const auto bucket=KeyRangeEnt::closed(0,255);
        ok(a.acquire(x1.id,LockMode::S,bucket,LockKind::Node),"S conversion setup");
        ok(b.acquire(x2.id,LockMode::IS,bucket,LockKind::Node),"peer IS conversion setup");
        check(a.acquire(x1.id,LockMode::IX,bucket,LockKind::Node).result==TxnResult::Conflict,"S+IX tests final X against IS");
        ok(a.end(x1.id),"conversion cleanup");ok(b.end(x2.id),"conversion peer cleanup");
    }
    {
        TransactionCoordinator e(1);PolicyClient a(e,1);
        const auto t=a.begin(WaitPolicy::NoWait,0,budget);
        ok(a.acquire(t.id,LockMode::S,x,LockKind::Record),"capacity setup");
        check(a.acquire(t.id,LockMode::S,y,LockKind::Record).result==TxnResult::Error,"capacity cannot grow under held locks");
        ok(a.end(t.id),"capacity cleanup");
    }
    for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait})
    for(auto protocol:{PolicyProtocol::Record,PolicyProtocol::Gap,PolicyProtocol::NextKey,PolicyProtocol::Hierarchy,PolicyProtocol::Predicate,PolicyProtocol::ExactRange}) {
        WorkloadConfig c;c.key_count=16;c.transaction_count=80;c.operations_per_transaction=4;c.threads=4;
        const auto trace=WorkloadTrace::generate(c);BTreeLatch index;index.init(8);LockManager physical;
        PolicyRunConfig cfg;cfg.wait_policy=policy;cfg.protocol=protocol;
        auto r=run_policy_workload(index,physical,trace,cfg);const auto& m=r.policy;
        if(r.failed())std::cerr<<"driver "<<static_cast<int>(protocol)<<" policy "<<wait_policy_name(policy)
            <<" lock/index/release errors "<<r.lock_errors<<'/'<<r.index_errors<<'/'<<r.lock_release_failures<<" tx "<<r.transactions<<std::endl;
        check(!r.failed() && index.validate() && physical.get_lock_count()==0,"driver clean success");
        check(r.transactions==r.committed+r.aborted+r.failed_transactions,"attempt conservation");
        check(m.logical_started==r.committed+m.unfinished+m.logical_failed,"logical conservation");
        check(m.logical_completion.count==r.committed && m.successful_attempt.count==r.committed,"two latency populations count every commit");
        check(r.transactions==m.logical_started+m.retries,"same logical txn is retried");
    }
    std::cout<<"Timestamp policies, commit/wound fence, cancellation, predicate and metrics: OK\n";
}
