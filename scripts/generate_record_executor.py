from pathlib import Path

raise SystemExit("Historical bootstrap only: record_workload.cpp is maintained directly; regeneration would remove subsequent optimizations.")

root = Path(__file__).resolve().parents[1]
original = (root / 'src/txn/policy_workload.cpp').read_text(encoding='utf-8')
tail = original[original.index('    try {\n        for(std::size_t w=0;w<os_workers;'):original.index('\nRunStats run_policy_workload(')]
# Preserve the existing barrier, warm-up measurement boundary, histogram schema
# and aggregate definitions. The transaction/lock execution below is separate.
tail = tail.replace('    RunStats result;auto& pm=result.policy;pm.enabled=true;', '''    const auto elapsed = ns(Clock::now()-start);
    RunStats result;auto& pm=result.policy;pm.enabled=true;
    pm.record_lock_bytes=table.lock_bytes();pm.record_directory_bytes=table.directory_bytes();
    pm.record_state_bytes=table.state_bytes();
    for(const auto& client:clients)pm.record_held_bytes+=client->held_bytes();
    if(!table.empty() && table.healthy())++result.lock_release_failures;''')
tail = tail.replace('result.elapsed_ns=ns(Clock::now()-start);', 'result.elapsed_ns=elapsed;')
head = r'''#include "txn/record_workload.h"
#include "lock/record_lock_table.h"
#include "txn/coroutine.h"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <new>
#include <thread>

namespace rrl {
namespace {
using Clock=std::chrono::steady_clock;
std::uint64_t ns(Clock::duration d) {return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());}
struct Worker {
    RunStats stats;
    LatencyHistogram attempt,logical,wait,call,release;
};

template<WaitPolicy Policy,bool Profile>
RunStats run_record_impl(BTreeIndex& index,const WorkloadTrace& trace,const PolicyRunConfig& cfg) {
    const auto total=trace.transactions().size();
    if(!total)return {};
    const auto worker_threads=trace.config().threads;
    if(cfg.protocol!=PolicyProtocol::Record || !worker_threads || !cfg.coroutines ||
       cfg.coroutines>4096 || worker_threads>4096/cfg.coroutines ||
       !cfg.max_attempts || !cfg.wait_timeout_ms || cfg.wait_timeout_ms>600000 || !cfg.lock_budget ||
       cfg.coroutine_stack_bytes<65536 || cfg.coroutine_stack_bytes>16777216 || cfg.warmup_seconds>600000 ||
       (!cfg.use_coroutines && cfg.coroutines!=1) || cfg.txn_state_port || !cfg.coordinator_host.empty() ||
       cfg.shared_local_state || cfg.native_object_locks || cfg.deneva_insert_behavior ||
       cfg.leaf_hierarchy || cfg.dag_hierarchy || cfg.separator_hierarchy ||
       cfg.hierarchy_escalation_threshold || cfg.hierarchy_deescalate_after)
        throw std::invalid_argument("invalid compact record execution configuration");
    if(!Profile && !cfg.histogram_path.empty())throw std::invalid_argument("record latency disabled with histogram output");
    const auto count=std::min(worker_threads*cfg.coroutines,total);
    const auto os_workers=std::min(worker_threads,count);
    if(count>RecordLockTable::max_contexts)throw std::invalid_argument("compact record table requires at most 64 contexts");
    std::unordered_map<idx_key_t,std::uint64_t> seed;
    if(!index.preloaded() && !cfg.index_preloaded && !seed_index(index,trace,seed)){RunStats s;s.index_errors=1;return s;}
    std::vector<idx_key_t> sparse;
    std::size_t maxops=0;
    for(const auto& txn:trace.transactions()) {
        maxops=std::max(maxops,txn.operations.size());
        for(const auto& op:txn.operations)
            if(op.kind!=OperationKind::RangeRead && op.key>=trace.config().key_count)sparse.push_back(op.key);
    }
    // Resource registration/preallocation is outside the measurement, like
    // trace generation and seed loading. Missing reads also have stable IDs.
    RecordLockTable table(trace.config().key_count,std::move(sparse),count);
    std::vector<std::unique_ptr<RecordLockTable::Client>> clients;
    for(std::size_t w=0;w<count;++w)clients.emplace_back(new RecordLockTable::Client(table,w,cfg.lock_budget));
    std::vector<Worker> parts(count);
    std::vector<std::vector<IndexMutation>> writes(count);
    std::vector<std::vector<KeyValue>> scans(count),rechecks(count);
    for(auto& v:writes)v.reserve(maxops);
    for(auto& v:scans)v.reserve(trace.config().range_length);
    for(auto& v:rechecks)v.reserve(trace.config().range_length);
    std::atomic<bool> stopped{false};
    Clock::time_point start,deadline;
    const bool warmup=cfg.warmup_seconds!=0;
    bool timed=warmup || trace.config().duration_seconds!=0;
    std::mutex start_mutex;std::condition_variable start_cv;
    std::size_t ready=0;
    std::atomic<unsigned> phase{0};
    std::vector<std::unique_ptr<CoroutineScheduler>> schedulers;
    if(cfg.use_coroutines)for(std::size_t w=0;w<os_workers;++w)
        schedulers.emplace_back(new CoroutineScheduler((count+os_workers-1-w)/os_workers,cfg.coroutine_stack_bytes));
    std::vector<std::thread> threads;
    auto work=[&](std::size_t worker) {
        auto& p=parts[worker];auto& s=p.stats;auto& m=s.policy;
        auto& client=*clients[worker];auto& pending=writes[worker];
        bool arrived=false,active_attempt=false;
        auto arrive=[&](unsigned target) {
            {std::lock_guard<std::mutex> guard(start_mutex);++ready;start_cv.notify_all();}
            while(phase.load(std::memory_order_acquire)<target)cooperative_yield();
        };
        try {
            index.prepare_thread(worker,trace.config().range_length);index.prepare_write_capacity(maxops);
            arrived=true;arrive(1);
            std::size_t cursor=worker;
            auto run_phase=[&] {
                for(std::size_t& i=cursor;!stopped.load(std::memory_order_relaxed) && (timed || i<total);i+=count) {
                    const auto logical_start=Clock::now();
                    if(logical_start>=deadline)break;
                    ++m.logical_started;
                    const auto& txn=trace.transactions()[i%total];
                    std::uint64_t age=0,retries=0;
                    bool completed=false,hard=false,attempted=false;
                    for(std::uint64_t attempt=0;attempt<cfg.max_attempts && !stopped.load(std::memory_order_relaxed);++attempt) {
                        const auto attempt_start=Clock::now();
                        if(attempt_start>=deadline)break;
                        attempted=true;++s.transactions;
                        if(attempt){++m.retries;++retries;}
                        auto budget=cfg.wait_timeout_ms*1000000ULL;
                        if(timed)budget=std::min<std::uint64_t>(budget,ns(deadline-attempt_start));
                        const auto begun=client.begin(age,budget);
                        if(begun.result!=TxnResult::Ok){++s.lock_errors;++s.failed_transactions;hard=true;stopped=true;break;}
                        age=begun.age;active_attempt=true;
                        const auto attempt_deadline=std::min(client.deadline(),deadline);
                        TxnResult outcome=TxnResult::Ok;
                        pending.clear();
                        auto own_write=[&](idx_key_t key)->const IndexMutation* {
                            for(auto it=pending.rbegin();it!=pending.rend();++it)if(it->key==key)return &*it;
                            return nullptr;
                        };
                        auto account_value=[&](idx_key_t key,const std::uint64_t* value) {
                            if(const auto* own=own_write(key)) {if(!own->remove){++s.rows_found;s.checksum+=key^*own->value;}}
                            else if(value){++s.rows_found;s.checksum+=key^*value;}
                        };
                        auto acquire=[&](idx_key_t key,LockMode mode) {
                            ++s.lock_acquires;
                            auto* record=table.locate(key);
                            if(!record){outcome=TxnResult::Error;return false;}
                            const auto reply=client.acquire<Policy>(*record,mode);
                            outcome=reply.result;
                            if constexpr(Profile)if(reply.wait_ns)p.wait.add(reply.wait_ns);
                            return outcome==TxnResult::Ok;
                        };
                        for(const auto& op:txn.operations) {
                            const auto call_start=Clock::now();
                            if(call_start>=attempt_deadline){outcome=TxnResult::Deadline;break;}
                            if constexpr(Policy==WaitPolicy::WoundWait) {
                                outcome=client.check();if(outcome!=TxnResult::Ok)break;
                            }
                            ++s.operations;
                            if(op.kind==OperationKind::RangeRead) {
                                ++s.range_reads;
                                if(op.end_key<=op.key)continue;
                                const auto range=KeyRangeEnt::half_open(op.key,op.end_key);
                                auto& rows=scans[worker];index.scan_range_into(range,rows);s.rows_scanned+=rows.size();
                                Clock::time_point lock_start;
                                if constexpr(Profile)lock_start=Clock::now();
                                for(const auto& row:rows)if(!acquire(row.key,LockMode::S))break;
                                if constexpr(Profile)if(!rows.empty())p.call.add(ns(Clock::now()-lock_start));
                                if(outcome!=TxnResult::Ok)break;
                                // Keep record-only scan semantics: newly appearing
                                // rows are NOT read without their S lock.
                                auto& verified=rechecks[worker];index.scan_range_into(range,verified);s.rows_rechecked+=verified.size();
                                std::size_t n=0;
                                for(const auto& row:rows) {
                                    while(n<verified.size() && verified[n].key<row.key)++n;
                                    if(n==verified.size() || verified[n].key!=row.key){outcome=TxnResult::Conflict;break;}
                                    account_value(row.key,static_cast<const std::uint64_t*>(verified[n].value));
                                }
                                if(outcome!=TxnResult::Ok)break;
                                for(const auto& own:pending) {
                                    if(own.remove || own_write(own.key)!=&own || !range.contains(own.key))continue;
                                    const auto found=std::lower_bound(verified.begin(),verified.end(),own.key,
                                        [](const KeyValue& row,idx_key_t key){return row.key<key;});
                                    if(found==verified.end() || found->key!=own.key)account_value(own.key,nullptr);
                                }
                            } else {
                                const bool read=op.kind==OperationKind::Read;
                                const bool granted=acquire(op.key,read?LockMode::S:LockMode::X);
                                if constexpr(Profile)p.call.add(ns(Clock::now()-call_start));
                                if(!granted)break;
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
                        if(outcome==TxnResult::Ok)outcome=client.prepare_commit<Policy>();
                        if(outcome==TxnResult::Ok && !pending.empty()) {
                            if(!index.healthy())throw std::runtime_error("data backend unhealthy before record commit");
                            const auto rc=index.apply_batch(pending);
                            if(rc==AbortCapacity){++s.lock_capacity_aborts;outcome=TxnResult::Conflict;}
                            else if(rc!=RCOK || !index.healthy())throw std::runtime_error("uncertain record writeback; retain locks");
                        }
                        Clock::time_point release_start;
                        if constexpr(Profile)release_start=Clock::now();
                        client.finish();active_attempt=false;
                        if constexpr(Profile)p.release.add(ns(Clock::now()-release_start));
                        if(client.capacity_abort())++s.lock_capacity_aborts;
                        if(outcome==TxnResult::Error){++s.lock_errors;++s.failed_transactions;hard=true;stopped=true;break;}
                        if(outcome==TxnResult::Ok) {
                            ++s.committed;completed=true;
                            if constexpr(Profile){const auto end=Clock::now();p.attempt.add(ns(end-attempt_start));p.logical.add(ns(end-logical_start));}
                            break;
                        }
                        ++s.aborted;
                        if(outcome==TxnResult::Conflict)++s.lock_conflicts;
                        if(outcome==TxnResult::Wounded)++m.wound_aborts;
                        if(outcome==TxnResult::Deadline)++m.deadline_aborts;
                        if(attempt+1==cfg.max_attempts)++m.retry_exhausted;
                        cooperative_yield();
                    }
                    if(!attempted){--m.logical_started;break;}
                    m.max_retries=std::max(m.max_retries,retries);
                    if(completed)m.completed_retries+=retries;else m.unfinished_retries+=retries;
                    if(hard)++m.logical_failed;else if(!completed)++m.unfinished;
                    // OS threads need no yield after an uncontended commit.
                    if(cfg.use_coroutines)cooperative_yield();
                }
            };
            if(warmup) {
                run_phase();const auto warm_stats=s;
                p.~Worker();::new(static_cast<void*>(&p))Worker;
                s.policy.warmup_committed=warm_stats.committed;s.policy.warmup_aborted=warm_stats.aborted;
                s.lock_errors=warm_stats.lock_errors;s.index_errors=warm_stats.index_errors;
                s.lock_release_failures=warm_stats.lock_release_failures;s.failed_transactions=warm_stats.failed_transactions;
                arrived=true;arrive(2);
            }
            if(!timed)cursor=worker;
            run_phase();
        } catch(const std::exception&) {
            table.poison();stopped=true;++s.index_errors;
            if(active_attempt){++s.failed_transactions;++m.logical_failed;}
            if(!arrived){arrived=true;arrive(1);}
            if(warmup && phase.load()<2)arrive(2);
        }
        index.finish_thread();
    };
'''
dispatch = r'''
} // namespace
RunStats run_compact_record_workload(BTreeIndex& index,const WorkloadTrace& trace,const PolicyRunConfig& cfg) {
#define RUN(P) return cfg.record_latency ? run_record_impl<P,true>(index,trace,cfg) : run_record_impl<P,false>(index,trace,cfg)
    switch(cfg.wait_policy) {
        case WaitPolicy::NoWait: RUN(WaitPolicy::NoWait);
        case WaitPolicy::WaitDie: RUN(WaitPolicy::WaitDie);
        case WaitPolicy::WoundWait: RUN(WaitPolicy::WoundWait);
    }
#undef RUN
    throw std::invalid_argument("invalid record wait policy");
}
} // namespace rrl
'''
(root / 'src/txn/record_workload.cpp').write_text(head + tail + dispatch, encoding='utf-8', newline='\n')
