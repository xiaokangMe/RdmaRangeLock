#include "txn/workload.h"
#include "txn/policy_workload.h"
#include "txn/coroutine.h"
#include "common/execution_context.h"
#include "lock/native_policy.h"
#include "hierarchical_plan.h"
#include "leaf_hierarchy.h"
#include <atomic>
#include <algorithm>
#include <thread>
#include <unordered_map>
#include <limits>
#include <fstream>
#include <stdexcept>
#include <condition_variable>
#include <new>

namespace rrl {
bool seed_index(BTreeIndex&,const WorkloadTrace&,std::unordered_map<idx_key_t,std::uint64_t>&);
namespace {
using Clock=std::chrono::steady_clock;
std::uint64_t ns(Clock::duration d) {return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());}
struct Worker {
    RunStats stats;
    std::uint64_t state_start{0};
    LatencyHistogram attempt,logical,wait,call,release;
};
KeyRangeEnt right_closed(idx_key_t a,idx_key_t b) {
    auto r=KeyRangeEnt::closed(a,b);r.start_bound=Bound::Open;return r;
}
}
RunStats run_policy_workload(BTreeIndex& index,IRangeLockManager& physical,const WorkloadTrace& trace,const PolicyRunConfig& cfg) {
    if((cfg.protocol==PolicyProtocol::None && cfg.wait_policy!=WaitPolicy::NoWait) ||
       !trace.config().threads || !cfg.max_attempts || !cfg.wait_timeout_ms || cfg.wait_timeout_ms>600000)throw std::invalid_argument("invalid retry/wait bound");
    std::unordered_map<idx_key_t,std::uint64_t> seed;
    if(!index.preloaded() && !seed_index(index,trace,seed)){RunStats s;s.index_errors=1;return s;}
    const auto total=trace.transactions().size();
    if(!total)return {};
    if(!cfg.coroutines || cfg.coroutines>4096 || trace.config().threads>4096/cfg.coroutines ||
       cfg.coroutine_stack_bytes<65536 || cfg.coroutine_stack_bytes>16777216 || cfg.warmup_seconds>600000)
        throw std::invalid_argument("runtime geometry bounds");
    if(cfg.coroutines>1 && !cfg.txn_state_port && !cfg.coordinator_host.empty())
        throw std::invalid_argument("TCP compatibility coordinator cannot block a coroutine worker");
    const auto count=std::min(trace.config().threads*cfg.coroutines,total);
    const auto os_workers=std::min(trace.config().threads,count);
    const bool warmup=cfg.warmup_seconds!=0;
    TransactionCoordinator local(cfg.lock_budget,cfg.leaf_hierarchy);
    std::vector<std::unique_ptr<PolicyClient>> clients;
    std::vector<std::unique_ptr<NativePolicyClient>> native;
    std::vector<Worker> parts(count);
    std::vector<std::vector<detail::HierarchyRequest>> plans;
    if(cfg.protocol==PolicyProtocol::Hierarchy && !cfg.leaf_hierarchy)for(const auto& t:trace.transactions())
        plans.push_back(detail::hierarchy_plan(t,trace.config().hierarchy_bucket_width));
    for(std::size_t w=0;w<count;++w) {
        if(cfg.protocol==PolicyProtocol::None)clients.emplace_back(nullptr);
        else if(cfg.txn_state_port) {
            native.emplace_back(new NativePolicyClient(cfg.coordinator_host,cfg.txn_state_port,physical));
            clients.emplace_back(nullptr);
        } else if(cfg.coordinator_host.empty())clients.emplace_back(new PolicyClient(local,w+1));
        else clients.emplace_back(new PolicyClient(cfg.coordinator_host,cfg.coordinator_port,cfg.wait_timeout_ms+5000));
    }
    std::vector<std::vector<IndexMutation>> writes(count);
    std::vector<std::vector<KeyValue>> scans(count),rechecks(count);
    const auto scan_capacity=trace.config().range_length;
    for(auto& v:scans)v.reserve(scan_capacity);
    for(auto& v:rechecks)v.reserve(scan_capacity);
    std::size_t maxops=0;for(const auto& t:trace.transactions())maxops=std::max(maxops,t.operations.size());
    for(auto& v:writes)v.reserve(maxops);
    std::vector<std::vector<LeafSpan>> leaf_scratch(count);
    std::vector<std::vector<detail::HierarchyRequest>> leaf_plans(count);
    if(cfg.leaf_hierarchy)for(std::size_t w=0;w<count;++w) {
        if(maxops && scan_capacity>std::numeric_limits<std::size_t>::max()/maxops-2)throw std::invalid_argument("leaf workspace overflow");
        const auto capacity=maxops*(scan_capacity+2)+2;
        leaf_scratch[w].reserve(scan_capacity+2);leaf_plans[w].reserve(capacity);
    }
    std::atomic<bool> stopped{false};
    Clock::time_point start,deadline;
    bool timed=warmup || trace.config().duration_seconds!=0;
    std::mutex start_mutex;std::condition_variable start_cv;
    std::size_t ready=0;
    std::atomic<unsigned> phase{0};
    std::vector<std::unique_ptr<CoroutineScheduler>> schedulers;
    for(std::size_t w=0;w<os_workers;++w) {
        const auto routines=(count+os_workers-1-w)/os_workers;
        schedulers.emplace_back(new CoroutineScheduler(routines,cfg.coroutine_stack_bytes));
    }
    std::vector<std::thread> threads;
    auto work=[&](std::size_t worker) {
        auto& p=parts[worker];auto& s=p.stats;auto& m=s.policy;
        auto* nc=cfg.txn_state_port && cfg.protocol!=PolicyProtocol::None ? native[worker].get() : nullptr;
        auto begin=[&](WaitPolicy policy,std::uint64_t age,std::uint64_t budget) {
            if(cfg.protocol==PolicyProtocol::None)return PolicyReply{TxnResult::Ok,worker+1,0,worker+1};
            return nc ? nc->begin(policy,age,budget) : clients[worker]->begin(policy,age,budget);
        };
        auto lock=[&](txnid_t id,LockMode mode,const KeyRangeEnt& r,LockKind kind) {
            return nc ? nc->acquire(id,mode,r,kind) : clients[worker]->acquire(id,mode,r,kind);
        };
        auto check=[&](txnid_t id){return nc ? nc->check(id) : clients[worker]->check(id);};
        auto prepare=[&](txnid_t id){if(cfg.protocol==PolicyProtocol::None)return PolicyReply{TxnResult::Ok,id,0};return nc ? nc->prepare_commit(id) : clients[worker]->prepare_commit(id);};
        auto end=[&](txnid_t id){if(cfg.protocol==PolicyProtocol::None)return PolicyReply{TxnResult::Ok,id,0};return nc ? nc->end(id) : clients[worker]->end(id);};
        auto arrive=[&](unsigned target) {
            {std::lock_guard<std::mutex> guard(start_mutex);++ready;start_cv.notify_all();}
            while(phase.load(std::memory_order_acquire)<target)cooperative_yield();
        };
        auto& pending=writes[worker];
        bool active_attempt=false,arrived=false;
        try {
        index.prepare_thread(worker,scan_capacity);index.prepare_write_capacity(maxops);
        arrived=true;arrive(1);
        std::size_t cursor=worker;
        auto run_phase=[&] {
        for(std::size_t& i=cursor;!stopped && (timed || i<total);i+=count) {
            if(Clock::now()>=deadline)break;
            ++m.logical_started;
            const auto logical_start=Clock::now();
            const auto& txn=trace.transactions()[i%total];
            std::uint64_t age=0,retries=0;
            bool completed=false,hard=false,attempted=false;
            bool structural=detail::topology_write(txn);
            for(std::uint64_t attempt=0;attempt<cfg.max_attempts && !stopped;++attempt) {
                const auto attempt_start=Clock::now();
                if(attempt_start>=deadline)break;
                attempted=true;++s.transactions;
                if(attempt){++m.retries;++retries;}
                auto budget=cfg.wait_timeout_ms*1000000ULL;
                if(timed)budget=std::min<std::uint64_t>(budget,ns(deadline-attempt_start));
                const auto begun=begin(cfg.wait_policy,age,budget);
                if(begun.result!=TxnResult::Ok){++s.lock_errors;++s.failed_transactions;hard=true;stopped=true;break;}
                const auto id=begun.id;age=begun.age;active_attempt=true;
                TxnResult outcome=TxnResult::Ok;
                bool touched=false;
                pending.clear();
                const auto own_write=[&](idx_key_t key)->const IndexMutation* {
                    for(auto it=pending.rbegin();it!=pending.rend();++it)if(it->key==key)return &*it;
                    return nullptr;
                };
                const auto account_value=[&](idx_key_t key,const std::uint64_t* fallback) {
                    if(const auto* own=own_write(key)) {
                        if(!own->remove){++s.rows_found;s.checksum+=key^*own->value;}
                    } else if(fallback){++s.rows_found;s.checksum+=key^*fallback;}
                };
                const auto account_inserted=[&](const KeyRangeEnt& range,const std::vector<KeyValue>& rows) {
                    for(const auto& own:pending) {
                        if(own.remove || own_write(own.key)!=&own || !range.contains(own.key))continue;
                        const auto found=std::lower_bound(rows.begin(),rows.end(),own.key,[](const KeyValue& r,idx_key_t k){return r.key<k;});
                        if(found==rows.end() || found->key!=own.key)account_value(own.key,nullptr);
                    }
                };
                auto acquire=[&](LockMode mode,const KeyRangeEnt& range,LockKind kind) {
                    if(Clock::now()>=deadline){outcome=TxnResult::Deadline;return false;}
                    ++s.lock_acquires;
                    const auto began=Clock::now();
                    // Native acquisition may grant then observe a wound.
                    // Always release its physical locks even on that outcome.
                    const auto r=lock(id,mode,range,kind);
                    if(nc)touched=nc->has_physical_locks();
                    if(nc && nc->last_lock_result()==AbortCapacity)++s.lock_capacity_aborts;
                    if(nc && nc->last_lock_result()==AbortGate)++s.lock_gate_aborts;
                    if(r.wait_ns)p.wait.add(r.wait_ns);
                    outcome=r.result;
                    if(outcome!=TxnResult::Ok){p.call.add(ns(Clock::now()-began));return false;}
                    touched=true;
                    const auto rc=nc ? RCOK : physical.lock(id,mode,range,kind);
                    p.call.add(ns(Clock::now()-began));
                    // Metadata grant excludes every coordinated conflict.
                    // A foreign/legacy client or physical-table failure is a
                    // hard error, never an unbounded client conflict retry.
                    if(rc!=RCOK){outcome=TxnResult::Error;return false;}
                    return true;
                };
                if(cfg.protocol==PolicyProtocol::Hierarchy) {
                    if(cfg.leaf_hierarchy) {
                        const bool writing=std::any_of(txn.operations.begin(),txn.operations.end(),[](const Operation& op){return op.kind!=OperationKind::Read && op.kind!=OperationKind::RangeRead;});
                        if(acquire(writing?LockMode::IX:LockMode::IS,KeyRangeEnt::point(0),LockKind::Node) &&
                           acquire(structural?LockMode::X:LockMode::S,KeyRangeEnt::point(1),LockKind::Node)) {
                            // apply_batch permits an absent Update to insert. A
                            // shared topology lease cannot authorize that path.
                            // Detect while topology is stable, then release and
                            // restart with X; never upgrade while holding S.
                            if(!structural)for(const auto& op:txn.operations)if(op.kind==OperationKind::Update) {
                                void* value=nullptr;
                                if(!index.lookup(op.key,value)){structural=true;outcome=TxnResult::Conflict;break;}
                            }
                            if(outcome==TxnResult::Ok) {
                                if(!detail::leaf_plan(index,txn,leaf_scratch[worker],leaf_plans[worker]))outcome=TxnResult::Error;
                                else for(const auto& r:leaf_plans[worker])if(!acquire(r.mode,r.range,r.kind))break;
                            }
                        }
                    }else for(const auto& r:plans[i%total])if(!acquire(r.mode,r.range,r.kind))break;
                }
                for(const auto& op:txn.operations) {
                    if(outcome!=TxnResult::Ok)break;
                    if(Clock::now()>=deadline){outcome=TxnResult::Deadline;break;}
                    if(cfg.wait_policy==WaitPolicy::WoundWait) {
                        outcome=check(id).result;
                        if(outcome!=TxnResult::Ok)break;
                    }
                    ++s.operations;
                    if(op.kind==OperationKind::RangeRead) {
                        ++s.range_reads;
                        if(op.end_key<=op.key)continue;
                        const auto range=KeyRangeEnt::half_open(op.key,op.end_key);
                        if(cfg.protocol==PolicyProtocol::Predicate && !acquire(LockMode::S,range,LockKind::Predicate))break;
                        if(cfg.protocol==PolicyProtocol::Gap) {
                            if(!acquire(LockMode::S,KeyRangeEnt::open(op.key,op.end_key),LockKind::Gap) ||
                               !acquire(LockMode::S,KeyRangeEnt::point(op.key),LockKind::Gap) ||
                               !acquire(LockMode::S,KeyRangeEnt::point(op.end_key),LockKind::Gap))break;
                        }
                        auto& rows=scans[worker];index.scan_range_into(range,rows);s.rows_scanned+=rows.size();
                        if(cfg.protocol==PolicyProtocol::NextKey) {
                            if(!acquire(LockMode::S,KeyRangeEnt::point(op.key),LockKind::NextKey))break;
                            auto previous=op.key;
                            for(const auto& row:rows) {
                                if(!acquire(LockMode::S,row.key==previous ? KeyRangeEnt::point(row.key) : right_closed(previous,row.key),LockKind::NextKey))break;
                                previous=row.key;
                            }
                            if(outcome!=TxnResult::Ok || !acquire(LockMode::S,right_closed(previous,op.end_key),LockKind::NextKey))break;
                            auto& verified=rechecks[worker];index.scan_range_into(range,verified);s.rows_rechecked+=verified.size();
                            if(verified.size()!=rows.size()){outcome=TxnResult::Conflict;break;}
                            for(std::size_t n=0;n<rows.size();++n)if(rows[n].key!=verified[n].key)outcome=TxnResult::Conflict;
                            if(outcome!=TxnResult::Ok)break;
                            for(const auto& row:verified)account_value(row.key,static_cast<const std::uint64_t*>(row.value));
                            account_inserted(range,verified);
                        } else if(cfg.protocol==PolicyProtocol::Record || cfg.protocol==PolicyProtocol::Gap) {
                            for(const auto& row:rows) {
                                if(!acquire(LockMode::S,KeyRangeEnt::point(row.key),LockKind::Record))break;
                                void* value=nullptr;
                                if(!index.lookup(row.key,value)){outcome=TxnResult::Conflict;break;}
                                account_value(row.key,static_cast<const std::uint64_t*>(value));
                            }
                            if(outcome==TxnResult::Ok)account_inserted(range,rows);
                        } else {
                            for(const auto& row:rows)account_value(row.key,static_cast<const std::uint64_t*>(row.value));
                            account_inserted(range,rows);
                        }
                    } else {
                        const bool read=op.kind==OperationKind::Read;
                        const auto kind=cfg.protocol==PolicyProtocol::Predicate ? LockKind::Predicate :
                            cfg.protocol==PolicyProtocol::NextKey ? LockKind::NextKey :
                            cfg.protocol==PolicyProtocol::Gap && op.kind==OperationKind::Insert ? LockKind::Gap : LockKind::Record;
                        if(cfg.protocol!=PolicyProtocol::Hierarchy && cfg.protocol!=PolicyProtocol::None && !acquire(read?LockMode::S:LockMode::X,KeyRangeEnt::point(op.key),kind))break;
                        if(read) {
                            ++s.reads;void* value=nullptr;
                            if(own_write(op.key))account_value(op.key,nullptr);
                            else if(index.lookup(op.key,value))account_value(op.key,static_cast<const std::uint64_t*>(value));
                        } else {
                            if(op.kind==OperationKind::Update)++s.updates;
                            else if(op.kind==OperationKind::Insert)++s.inserts;
                            else ++s.deletes;
                            pending.push_back({op.key,&op.value,op.kind==OperationKind::Delete});
                        }
                    }
                }
                if(!index.healthy())throw std::runtime_error("data backend failed; retain active transaction");
                if(outcome==TxnResult::Ok)outcome=prepare(id).result;
                if(outcome==TxnResult::Ok) {
                    const auto rc=index.apply_batch(pending);
                    if(rc==AbortCapacity){++s.lock_capacity_aborts;outcome=TxnResult::Conflict;}
                    else if(rc!=RCOK){++s.index_errors;outcome=TxnResult::Error;}
                }
                // A failed/uncertain data mutation keeps its transaction active:
                // disconnect poisons the cohort; never grant over partial data.
                if(!index.healthy())throw std::runtime_error("uncertain shared write; retain locks");
                const auto release_start=Clock::now();
                if(touched && physical.unlock_all(id)!=RCOK){++s.lock_release_failures;outcome=TxnResult::Error;}
                if(!s.lock_release_failures && end(id).result!=TxnResult::Ok){++s.lock_release_failures;outcome=TxnResult::Error;}
                p.release.add(ns(Clock::now()-release_start));
                active_attempt=false;
                if(outcome==TxnResult::Error) {
                    if(!s.index_errors && !s.lock_release_failures)++s.lock_errors;
                    ++s.failed_transactions;hard=true;stopped=true;break;
                }
                if(outcome==TxnResult::Ok) {
                    ++s.committed;completed=true;
                    p.attempt.add(ns(Clock::now()-attempt_start));p.logical.add(ns(Clock::now()-logical_start));break;
                }
                ++s.aborted;
                if(outcome==TxnResult::Conflict)++s.lock_conflicts;
                if(outcome==TxnResult::Wounded)++m.wound_aborts;
                if(outcome==TxnResult::Deadline)++m.deadline_aborts;
                if(attempt+1==cfg.max_attempts)++m.retry_exhausted;
                // Like Deneva's return to the worker after abort: let the
                // holder/victim run before this same logical txn retries.
                cooperative_yield();
            }
            if(!attempted){--m.logical_started;break;}
            m.max_retries=std::max(m.max_retries,retries);
            if(completed)m.completed_retries+=retries;
            else m.unfinished_retries+=retries;
            if(hard)++m.logical_failed;
            else if(!completed)++m.unfinished;
            cooperative_yield();
        }
        };
        if(warmup) {
            run_phase();
            const auto warm_stats=s;
            // Construct in place: a Worker contains five large histograms.
            // A temporary would overflow a valid 64 KiB coroutine stack.
            p.~Worker();::new (static_cast<void*>(&p)) Worker;
            s.policy.warmup_committed=warm_stats.committed;
            s.policy.warmup_aborted=warm_stats.aborted;
            // Warm-up failures invalidate the whole case, never erase errors.
            s.lock_errors=warm_stats.lock_errors;s.index_errors=warm_stats.index_errors;
            s.lock_release_failures=warm_stats.lock_release_failures;
            s.failed_transactions=warm_stats.failed_transactions;
            arrived=true;arrive(2);
        }
        if(nc)p.state_start=nc->rdma_operations();
        // Finite trace runs get the full transaction population after warm-up.
        if(!timed)cursor=worker;
        run_phase();
        } catch(const std::exception&) {
            ++s.index_errors;stopped=true;
            if(!arrived) {arrived=true;arrive(1);}
            if(warmup && phase.load()<2)arrive(2);
            if(active_attempt){++s.failed_transactions;++m.logical_failed;}
        }
        if(nc)s.policy.state_rdma_operations=nc->rdma_operations()-p.state_start;
        index.finish_thread();
    };
    try {
        for(std::size_t w=0;w<os_workers;++w)threads.emplace_back([&,w] {
            schedulers[w]->run([&](std::size_t c){work(w+c*os_workers);});
        });
    } catch(...) {
        stopped=true;
        // Workers may still be preparing; publish both stop phases and join
        // every successfully launched OS thread before propagating the error.
        phase.store(2,std::memory_order_release);
        for(auto& thread:threads)thread.join();
        throw;
    }
    std::uint64_t warmup_elapsed=0;
    std::exception_ptr setup_error;
    {
        std::unique_lock<std::mutex> guard(start_mutex);
        start_cv.wait(guard,[&]{return ready==count;});
        start=Clock::now();
        deadline=warmup ? start+std::chrono::seconds(cfg.warmup_seconds) :
            timed ? start+std::chrono::seconds(trace.config().duration_seconds) : Clock::time_point::max();
        if(!warmup && !stopped && cfg.measurement_begin) {
            try{cfg.measurement_begin();}catch(...){stopped=true;setup_error=std::current_exception();}
            start=Clock::now();
            deadline=timed ? start+std::chrono::seconds(trace.config().duration_seconds) : Clock::time_point::max();
        }
        phase.store(1,std::memory_order_release);
        if(warmup) {
            start_cv.wait(guard,[&]{return ready==2*count;});
            warmup_elapsed=ns(Clock::now()-start);
            if(!stopped && cfg.measurement_begin) {
                try{cfg.measurement_begin();}catch(...){stopped=true;setup_error=std::current_exception();}
            }
            timed=trace.config().duration_seconds!=0;
            start=Clock::now();
            deadline=timed ? start+std::chrono::seconds(trace.config().duration_seconds) : Clock::time_point::max();
            phase.store(2,std::memory_order_release);
        }
    }
    for(auto& t:threads)t.join();
    if(setup_error)std::rethrow_exception(setup_error);
    RunStats result;auto& pm=result.policy;pm.enabled=true;
    pm.warmup_elapsed_ns=warmup_elapsed;
    for(const auto& scheduler:schedulers)pm.coroutine_switches+=scheduler->switches();
    LatencyHistogram attempt,logical,wait,call,release;
    for(const auto& p:parts) {
        const auto& s=p.stats;const auto& m=s.policy;
#define SUM(f) result.f+=s.f
        SUM(transactions);SUM(committed);SUM(aborted);SUM(operations);SUM(reads);SUM(updates);SUM(inserts);SUM(deletes);
        SUM(range_reads);SUM(rows_found);SUM(rows_scanned);SUM(rows_rechecked);SUM(lock_acquires);SUM(lock_conflicts);SUM(checksum);
        SUM(lock_errors);SUM(index_errors);SUM(lock_release_failures);SUM(failed_transactions);
        SUM(lock_capacity_aborts);SUM(lock_gate_aborts);
#undef SUM
        pm.warmup_committed+=m.warmup_committed;pm.warmup_aborted+=m.warmup_aborted;
        pm.state_rdma_operations+=m.state_rdma_operations;
        pm.logical_started+=m.logical_started;pm.unfinished+=m.unfinished;pm.logical_failed+=m.logical_failed;
        pm.retries+=m.retries;pm.max_retries=std::max(pm.max_retries,m.max_retries);
        pm.completed_retries+=m.completed_retries;pm.unfinished_retries+=m.unfinished_retries;
        pm.wound_aborts+=m.wound_aborts;pm.deadline_aborts+=m.deadline_aborts;pm.retry_exhausted+=m.retry_exhausted;
        attempt.merge(p.attempt);logical.merge(p.logical);wait.merge(p.wait);call.merge(p.call);release.merge(p.release);
    }
    pm.successful_attempt=attempt.summary();pm.logical_completion=logical.summary();pm.lock_wait=wait.summary();
    pm.lock_call=call.summary();pm.release=release.summary();
    result.elapsed_ns=ns(Clock::now()-start);
    // Histograms are written only after every worker has released its locks,
    // and outside the measurement window. CSV remains a separate channel.
    if(!cfg.histogram_path.empty() && !result.failed()) {
        std::ofstream out(cfg.histogram_path);
        if(!out)throw std::runtime_error("cannot write latency histogram");
        out << "{\"schema\":1,\"wait_policy\":\"" << wait_policy_name(cfg.wait_policy) << "\",\"histograms\":{";
        out << "\"successful_attempt\":";attempt.write_json(out);
        out << ",\"logical_completion\":";logical.write_json(out);
        out << ",\"lock_wait\":";wait.write_json(out);
        out << ",\"lock_call\":";call.write_json(out);
        out << ",\"release\":";release.write_json(out);
        out << "}}\n";
        if(!out)throw std::runtime_error("latency histogram write failed");
    }
    return result;
}
} // namespace rrl
