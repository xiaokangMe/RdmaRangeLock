#include "txn/record_workload.h"
#include "txn/retry_backoff.h"
#include "lock/record_lock_table.h"
#include "lock/classic_lock_table.h"
#include "lock/hierarchy_lock_table.h"
#include "compact_hierarchy_plan.h"
#include "compact_exact_plan.h"
#include "key_range_plan.h"
#include "txn/coroutine.h"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>

namespace rrl {
namespace {
using Clock=std::chrono::steady_clock;
std::uint64_t ns(Clock::duration d) {return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());}
struct Worker {
    RunStats stats;
    std::unique_ptr<LatencyHistogram> attempt,logical,wait,call,release;
};

template<WaitPolicy Policy,bool Profile,bool Statistics,PolicyProtocol Protocol=PolicyProtocol::Record>
RunStats run_record_impl(BTreeIndex& index,const WorkloadTrace& trace,const PolicyRunConfig& cfg) {
    const auto total=trace.transactions().size();
    if(!total)return {};
    if(cfg.retry_backoff!="off" && cfg.retry_backoff!="adaptive")throw std::invalid_argument("retry-backoff must be off or adaptive");
    std::vector<unsigned char> transaction_writes(total);
    for(std::size_t n=0;n<total;++n)for(const auto& op:trace.transactions()[n].operations)
        if(op.kind!=OperationKind::Read && op.kind!=OperationKind::RangeRead){transaction_writes[n]=1;break;}
    const auto worker_threads=trace.config().threads;
    if(cfg.protocol!=Protocol || !worker_threads || !cfg.coroutines ||
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
    if constexpr(Protocol==PolicyProtocol::ExactRange)
        if(count>63)throw std::invalid_argument("compact exact-range requires at most 63 contexts for exact shared compatibility");
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
    std::unique_ptr<ClassicLockTable> classic;
    std::unique_ptr<RecordLockTable> records;
    std::unique_ptr<HierarchyLockTable> hierarchy;
    std::unique_ptr<detail::CompactHierarchyPlans> hierarchy_plans;
    std::unique_ptr<detail::CompactExactPlans> exact_plans;
    if constexpr(Protocol==PolicyProtocol::Hierarchy) {
        hierarchy_plans=std::make_unique<detail::CompactHierarchyPlans>(trace,cfg.hierarchy_implicit_root);
        hierarchy=std::make_unique<HierarchyLockTable>(hierarchy_plans->nodes,count,Policy,cfg.hierarchy_padded);
    } else if constexpr(Protocol==PolicyProtocol::ExactRange) {
        exact_plans=std::make_unique<detail::CompactExactPlans>(trace);
        records=std::make_unique<RecordLockTable>(exact_plans->nodes,std::vector<idx_key_t>{},count);
    } else if constexpr(Protocol==PolicyProtocol::Record) {
        records=std::make_unique<RecordLockTable>(trace.config().key_count,std::move(sparse),count);
    } else {
        std::vector<idx_key_t> coordinates;
        coordinates.reserve(trace.config().key_count);
        for(std::size_t k=0;k<trace.config().key_count;++k)coordinates.push_back(k);
        // Include preloaded resources as well as all keys the benchmark admits.
        // This is a fixed-key-universe specialization, not conflict prediction.
        if(index.preloaded() || cfg.index_preloaded) {
            std::vector<idx_key_t> loaded;
            if(!index.key_range_into(KeyRangeEnt::unbounded(),loaded))throw std::invalid_argument("classic index has no key directory");
            coordinates.insert(coordinates.end(),loaded.begin(),loaded.end());
        }
        for(const auto& txn:trace.transactions())for(const auto& op:txn.operations) {
            coordinates.push_back(op.key);
            if(op.kind==OperationKind::RangeRead)coordinates.push_back(op.end_key);
        }
        classic=std::make_unique<ClassicLockTable>(std::move(coordinates),count,Protocol);
    }
    using Authority=std::conditional_t<Protocol==PolicyProtocol::Hierarchy,HierarchyLockTable,RecordLockTable>;
    using Client=typename Authority::Client;
    Authority& table=[&]()->Authority& {
        if constexpr(Protocol==PolicyProtocol::Hierarchy)return *hierarchy;
        else return classic?classic->authority():*records;
    }();
    std::size_t held_budget=0;
    if(maxops && trace.config().range_length+1 > SIZE_MAX/maxops)held_budget=cfg.lock_budget;
    else held_budget=std::min<std::size_t>(cfg.lock_budget,maxops*(trace.config().range_length+1));
    held_budget=std::max<std::size_t>(held_budget,1);
    if constexpr(Protocol!=PolicyProtocol::Record) {
        // An interval includes point and gap atoms; long sparse gaps can span
        // more coordinates than scan rows. Reserve up to the explicit budget.
        held_budget=std::min<std::size_t>(cfg.lock_budget,table.lock_bytes()/8);
    }
    if constexpr(Protocol==PolicyProtocol::Hierarchy)held_budget=std::min(cfg.lock_budget,hierarchy_plans->max_locks);
    if constexpr(Protocol==PolicyProtocol::ExactRange)held_budget=std::min(cfg.lock_budget,exact_plans->max_locks);
    std::vector<std::unique_ptr<Client>> clients;
    for(std::size_t w=0;w<count;++w)clients.emplace_back(new Client(table,w,held_budget));
    std::vector<Worker> parts(count);
    if constexpr(Profile)for(auto& part:parts) {
        part.attempt=std::make_unique<LatencyHistogram>();part.logical=std::make_unique<LatencyHistogram>();
        part.wait=std::make_unique<LatencyHistogram>();part.call=std::make_unique<LatencyHistogram>();
        part.release=std::make_unique<LatencyHistogram>();
    }
    std::vector<std::vector<IndexMutation>> writes(count);
    std::vector<std::vector<KeyValue>> scans(count),rechecks(count);
    std::vector<detail::KeyObservation> before(count),after(count);
    std::vector<std::vector<detail::KeyRequest>> key_plans(count);
    for(auto& v:writes)v.reserve(maxops);
    for(auto& v:scans)v.reserve(trace.config().range_length);
    for(auto& v:rechecks)v.reserve(trace.config().range_length);
    if constexpr(Protocol!=PolicyProtocol::Record && Protocol!=PolicyProtocol::Hierarchy && Protocol!=PolicyProtocol::ExactRange)for(std::size_t w=0;w<count;++w) {
        before[w].keys.reserve(trace.config().range_length);
        after[w].keys.reserve(trace.config().range_length);
        key_plans[w].reserve(2*trace.config().range_length+8);
    }
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
        detail::RetryBackoff backoff(worker,cfg.retry_backoff=="adaptive",
            Protocol==PolicyProtocol::Hierarchy && Policy==WaitPolicy::NoWait?6:2);
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
                        // No-wait never compares transaction ages. Avoid the
                        // shared age cache line in the specialized key path.
                        const auto begun=[&] {
                            if constexpr(Protocol!=PolicyProtocol::Record && Policy==WaitPolicy::NoWait)
                                return client.begin_unordered(budget);
                            else return client.begin(age,budget);
                        }();
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
                            if(const auto* own=own_write(key)) {if(!own->remove){if constexpr(Statistics){++s.rows_found;s.checksum+=key^*own->value;}}}
                            else if(value){if constexpr(Statistics){++s.rows_found;s.checksum+=key^*value;}}
                        };
                        auto acquire=[&](idx_key_t key,LockMode mode) {
                            if constexpr(Statistics)++s.lock_acquires;
                            auto* record=table.locate(key);
                            if(!record){outcome=TxnResult::Error;return false;}
                            const auto reply=client.template acquire<Policy>(*record,mode);
                            outcome=reply.result;
                            if constexpr(Profile)if(reply.wait_ns)p.wait->add(reply.wait_ns);
                            return outcome==TxnResult::Ok;
                        };
                        if constexpr(Protocol==PolicyProtocol::Hierarchy) {
                            Clock::time_point lock_start;if constexpr(Profile)lock_start=Clock::now();unsigned acquired=0;
                            for(const auto& request:hierarchy_plans->plans[i%total]) {
                                if((acquired++&31)==0){outcome=client.check();if(outcome!=TxnResult::Ok)break;}
                                if constexpr(Statistics)++s.lock_acquires;
                                const auto reply=client.template acquire_planned<Policy>(request.node,request.mode);outcome=reply.result;
                                if constexpr(Profile)if(reply.wait_ns)p.wait->add(reply.wait_ns);
                                if(outcome!=TxnResult::Ok)break;
                            }
                            if constexpr(Profile)p.call->add(ns(Clock::now()-lock_start));
                        }
                        if constexpr(Protocol==PolicyProtocol::ExactRange) {
                            Clock::time_point lock_start;if constexpr(Profile)lock_start=Clock::now();
                            unsigned acquired=0;
                            for(const auto& request:exact_plans->plans[i%total]) {
                                if((acquired++&31)==0){outcome=client.check();if(outcome!=TxnResult::Ok)break;}
                                if constexpr(Statistics)++s.lock_acquires;
                                const auto reply=client.template acquire<Policy>(*table.locate(request.node),request.mode);
                                outcome=reply.result;
                                if constexpr(Profile)if(reply.wait_ns)p.wait->add(reply.wait_ns);
                                if(outcome!=TxnResult::Ok)break;
                            }
                            if constexpr(Profile)p.call->add(ns(Clock::now()-lock_start));
                        }
                        for(const auto& op:txn.operations) {
                            if(outcome!=TxnResult::Ok)break;
                            const auto call_start=Clock::now();
                            if(call_start>=attempt_deadline){outcome=TxnResult::Deadline;break;}
                            if constexpr(Policy==WaitPolicy::WoundWait) {
                                outcome=client.check();if(outcome!=TxnResult::Ok)break;
                            }
                            if constexpr(Statistics)++s.operations;
                            if constexpr(Protocol==PolicyProtocol::Hierarchy || Protocol==PolicyProtocol::ExactRange) {
                                // ExactRange prelocks the exact interval partition;
                                // Hierarchy prelocks covering buckets. Both protect empty gaps.
                                // The preacquired bucket S/SIX protects scan
                                // membership, including empty gaps. No neighbor
                                // planning or record-lock enumeration is needed.
                                if(op.kind==OperationKind::RangeRead) {
                                    if constexpr(Statistics)++s.range_reads;
                                    if(op.end_key<=op.key)continue;
                                    const auto range=KeyRangeEnt::half_open(op.key,op.end_key);
                                    auto& rows=scans[worker];index.scan_range_into(range,rows);
                                    if constexpr(Statistics)s.rows_scanned+=rows.size();
                                    for(const auto& row:rows)account_value(row.key,static_cast<const std::uint64_t*>(row.value));
                                    for(const auto& own:pending) {
                                        if(own.remove || own_write(own.key)!=&own || !range.contains(own.key))continue;
                                        const auto found=std::lower_bound(rows.begin(),rows.end(),own.key,
                                            [](const KeyValue& row,idx_key_t key){return row.key<key;});
                                        if(found==rows.end() || found->key!=own.key)account_value(own.key,nullptr);
                                    }
                                } else if(op.kind==OperationKind::Read) {
                                    if constexpr(Statistics)++s.reads;
                                    void* value=nullptr;
                                    if(own_write(op.key))account_value(op.key,nullptr);
                                    else if(index.lookup(op.key,value))account_value(op.key,static_cast<const std::uint64_t*>(value));
                                } else {
                                    if constexpr(Statistics) {
                                        if(op.kind==OperationKind::Update)++s.updates;
                                        else if(op.kind==OperationKind::Insert)++s.inserts;
                                        else ++s.deletes;
                                    }
                                    pending.push_back({op.key,&op.value,op.kind==OperationKind::Delete});
                                }
                                continue;
                            } else if constexpr(Protocol!=PolicyProtocol::Record) {
                                if(!detail::observe_keys(index,op,before[worker])) {outcome=TxnResult::Conflict;break;}
                                auto& plan=key_plans[worker];
                                if constexpr(Protocol==PolicyProtocol::NextKey || Protocol==PolicyProtocol::PriorKey) {
                                    if(op.kind==OperationKind::RangeRead) {
                                        // All scan units are adjacent. Build their union
                                        // directly, without one request per physical row.
                                        plan.clear();
                                        if(op.key<op.end_key) {
                                            const auto& n=before[worker].lower;const auto& u=before[worker].upper;
                                            KeyRangeEnt span;
                                            span.has_start=n.has_predecessor;span.StartKey=n.predecessor;
                                            span.has_end=u.present || u.has_successor;
                                            span.EndKey=u.present?op.end_key:u.successor;
                                            span.start_bound=Protocol==PolicyProtocol::NextKey?Bound::Open:Bound::Closed;
                                            span.end_bound=Protocol==PolicyProtocol::NextKey?Bound::Closed:Bound::Open;
                                            if constexpr(Protocol==PolicyProtocol::PriorKey)
                                                if(n.present){span.has_start=true;span.StartKey=op.key;}
                                            if(span.valid())plan.push_back({LockMode::S,span,
                                                Protocol==PolicyProtocol::NextKey?LockKind::NextKey:LockKind::PriorKey});
                                        }
                                    } else detail::key_plan(op,before[worker],Protocol,plan);
                                } else detail::key_plan(op,before[worker],Protocol,plan);
                                for(std::size_t request=0;request<plan.size();) {
                                    auto range=plan[request].range;
                                    const auto mode=plan[request].mode;const auto kind=plan[request].kind;
                                    ++request;
                                    if constexpr(Protocol!=PolicyProtocol::Gap) {
                                        // Adjacent equal-mode intervals have identical union;
                                        // take each atomic cell once for a complete scan.
                                        while(request<plan.size() && plan[request].mode==mode && plan[request].kind==kind &&
                                              range.has_end && plan[request].range.has_start &&
                                              range.EndKey==plan[request].range.StartKey &&
                                              (range.end_bound==Bound::Closed || plan[request].range.start_bound==Bound::Closed)) {
                                            range.has_end=plan[request].range.has_end;
                                            range.EndKey=plan[request].range.EndKey;range.end_bound=plan[request].range.end_bound;++request;
                                        }
                                    }
                                    if constexpr(Statistics)++s.lock_acquires;
                                    const auto r=classic->acquire<Policy>(client,mode,range,kind);outcome=r.result;
                                    if constexpr(Profile)if(r.wait_ns)p.wait->add(r.wait_ns);
                                    if(outcome!=TxnResult::Ok)break;
                                }
                                if constexpr(Profile)p.call->add(ns(Clock::now()-call_start));
                                if(outcome!=TxnResult::Ok)break;
                                if(!detail::observe_keys(index,op,after[worker]) ||
                                   !detail::same_observation(before[worker],after[worker],op.kind==OperationKind::RangeRead)) {
                                    outcome=TxnResult::Conflict;break;
                                }
                                if(op.kind==OperationKind::RangeRead) {
                                    if constexpr(Statistics)++s.range_reads;
                                    if(op.end_key<=op.key)continue;
                                    const auto range=KeyRangeEnt::half_open(op.key,op.end_key);
                                    auto& rows=scans[worker];index.scan_range_into(range,rows);
                                    if constexpr(Statistics)s.rows_scanned+=rows.size();
                                    for(const auto& row:rows)account_value(row.key,static_cast<const std::uint64_t*>(row.value));
                                    for(const auto& own:pending) {
                                        if(own.remove || own_write(own.key)!=&own || !range.contains(own.key))continue;
                                        const auto it=std::lower_bound(rows.begin(),rows.end(),own.key,
                                            [](const KeyValue& row,idx_key_t key){return row.key<key;});
                                        if(it==rows.end() || it->key!=own.key)account_value(own.key,nullptr);
                                    }
                                } else if(op.kind==OperationKind::Read) {
                                    if constexpr(Statistics)++s.reads;
                                    void* value=nullptr;
                                    if(own_write(op.key))account_value(op.key,nullptr);
                                    else if(index.lookup(op.key,value))account_value(op.key,static_cast<const std::uint64_t*>(value));
                                } else {
                                    if constexpr(Statistics) {
                                        if(op.kind==OperationKind::Update)++s.updates;
                                        else if(op.kind==OperationKind::Insert)++s.inserts;
                                        else ++s.deletes;
                                    }
                                    pending.push_back({op.key,&op.value,op.kind==OperationKind::Delete});
                                }
                                continue;
                            }
                            if(op.kind==OperationKind::RangeRead) {
                                if constexpr(Statistics)++s.range_reads;
                                if(op.end_key<=op.key)continue;
                                const auto range=KeyRangeEnt::half_open(op.key,op.end_key);
                                auto& rows=scans[worker];index.scan_range_into(range,rows);if constexpr(Statistics)s.rows_scanned+=rows.size();
                                Clock::time_point lock_start;
                                if constexpr(Profile)lock_start=Clock::now();
                                for(const auto& row:rows)if(!acquire(row.key,LockMode::S))break;
                                if constexpr(Profile)if(!rows.empty())p.call->add(ns(Clock::now()-lock_start));
                                if(outcome!=TxnResult::Ok)break;
                                // Keep record-only scan semantics: newly appearing
                                // rows are NOT read without their S lock.
                                auto& verified=rechecks[worker];index.scan_range_into(range,verified);if constexpr(Statistics)s.rows_rechecked+=verified.size();
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
                                if constexpr(Profile)p.call->add(ns(Clock::now()-call_start));
                                if(!granted)break;
                                if(read) {
                                    if constexpr(Statistics)++s.reads;
                                    void* value=nullptr;
                                    if(own_write(op.key))account_value(op.key,nullptr);
                                    else if(index.lookup(op.key,value))account_value(op.key,static_cast<const std::uint64_t*>(value));
                                } else {
                                    if constexpr(Statistics) {
                                        if(op.kind==OperationKind::Update)++s.updates;
                                        else if(op.kind==OperationKind::Insert)++s.inserts;
                                        else ++s.deletes;
                                    }
                                    pending.push_back({op.key,&op.value,op.kind==OperationKind::Delete});
                                }
                            }
                        }
                        if(outcome==TxnResult::Ok)outcome=client.template prepare_commit<Policy>();
                        if(outcome==TxnResult::Ok && !pending.empty()) {
                            if(!index.healthy())throw std::runtime_error("data backend unhealthy before record commit");
                            const auto rc=index.apply_batch(pending);
                            if(rc==AbortCapacity){++s.lock_capacity_aborts;outcome=TxnResult::Conflict;}
                            else if(rc!=RCOK || !index.healthy())throw std::runtime_error("uncertain record writeback; retain locks");
                        }
                        Clock::time_point release_start;
                        if constexpr(Profile)release_start=Clock::now();
                        client.finish();active_attempt=false;
                        if constexpr(Profile)p.release->add(ns(Clock::now()-release_start));
                        if(client.capacity_abort())++s.lock_capacity_aborts;
                        if(outcome==TxnResult::Error){++s.lock_errors;++s.failed_transactions;hard=true;stopped=true;break;}
                        if(outcome==TxnResult::Ok) {
                            ++s.committed;completed=true;
                            if(transaction_writes[i%total])++m.committed_with_writes;else ++m.committed_read_only;
                            if constexpr(Profile){const auto end=Clock::now();p.attempt->add(ns(end-attempt_start));p.logical->add(ns(end-logical_start));}
                            break;
                        }
                        ++s.aborted;
                        if(outcome==TxnResult::Conflict)++s.lock_conflicts;
                        if(outcome==TxnResult::Wounded)++m.wound_aborts;
                        if(outcome==TxnResult::Deadline)++m.deadline_aborts;
                        if(attempt+1==cfg.max_attempts)++m.retry_exhausted;
                        else if(!client.capacity_abort()) {
                            // finish() above has released every lock. A failed
                            // context must not immediately attack the winner.
                            const auto paused=backoff.pause(attempt+1,deadline);
                            if(paused){++m.retry_backoff_count;m.retry_backoff_ns+=paused;}
                        }
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
                p.stats=RunStats{};
                if constexpr(Protocol==PolicyProtocol::Hierarchy)client.reset_metrics();
                if constexpr(Profile) {
                    *p.attempt=LatencyHistogram{};*p.logical=LatencyHistogram{};
                    *p.wait=LatencyHistogram{};*p.call=LatencyHistogram{};*p.release=LatencyHistogram{};
                }
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
    try {
        for(std::size_t w=0;w<os_workers;++w)threads.emplace_back([&,w] {
            if(cfg.use_coroutines)schedulers[w]->run([&](std::size_t c){work(w+c*os_workers);});
            else work(w);
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
    const auto elapsed = ns(Clock::now()-start);
    RunStats result;auto& pm=result.policy;pm.enabled=true;
    pm.record_lock_bytes=table.lock_bytes();pm.record_directory_bytes=table.directory_bytes();
    if(classic)pm.record_directory_bytes+=classic->directory_bytes();
    if(exact_plans)pm.exact_plan_bytes=exact_plans->plan_bytes;
    pm.record_state_bytes=table.state_bytes();
    for(const auto& client:clients)pm.record_held_bytes+=client->held_bytes();
    if constexpr(Protocol==PolicyProtocol::Hierarchy) {
        pm.hierarchy_lease_bytes=table.lease_bytes();pm.hierarchy_plan_bytes=hierarchy_plans->plan_bytes;
        for(const auto& client:clients)pm.hierarchy_cas_retries+=client->cas_retries();
    }
    if(!table.empty() && table.healthy())++result.lock_release_failures;
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
        pm.escalation_attempts+=m.escalation_attempts;pm.escalations+=m.escalations;pm.escalation_failures+=m.escalation_failures;
        pm.deescalation_attempts+=m.deescalation_attempts;pm.deescalations+=m.deescalations;pm.deescalation_failures+=m.deescalation_failures;
        pm.warmup_committed+=m.warmup_committed;pm.warmup_aborted+=m.warmup_aborted;
        pm.state_rdma_operations+=m.state_rdma_operations;
        pm.index_pending_aborts+=m.index_pending_aborts;
        if(m.logical_started) {
            if(!pm.active_workers)pm.worker_min_committed=s.committed;
            else pm.worker_min_committed=std::min(pm.worker_min_committed,s.committed);
            ++pm.active_workers;
            if(!s.committed)++pm.workers_without_commit;
            pm.worker_max_committed=std::max(pm.worker_max_committed,s.committed);
        }
        pm.deneva_early_inserts+=m.deneva_early_inserts;pm.deneva_early_unlocks+=m.deneva_early_unlocks;
        pm.deneva_split_aborts+=m.deneva_split_aborts;pm.deneva_partial_write_aborts+=m.deneva_partial_write_aborts;
        pm.logical_started+=m.logical_started;pm.unfinished+=m.unfinished;pm.logical_failed+=m.logical_failed;
        pm.retries+=m.retries;pm.max_retries=std::max(pm.max_retries,m.max_retries);
        pm.retry_backoff_count+=m.retry_backoff_count;pm.retry_backoff_ns+=m.retry_backoff_ns;
        pm.committed_read_only+=m.committed_read_only;pm.committed_with_writes+=m.committed_with_writes;
        pm.completed_retries+=m.completed_retries;pm.unfinished_retries+=m.unfinished_retries;
        pm.wound_aborts+=m.wound_aborts;pm.deadline_aborts+=m.deadline_aborts;pm.retry_exhausted+=m.retry_exhausted;
        if constexpr(Profile) {
            attempt.merge(*p.attempt);logical.merge(*p.logical);wait.merge(*p.wait);call.merge(*p.call);release.merge(*p.release);
        }
    }
    if constexpr(Profile) {
        pm.successful_attempt=attempt.summary();pm.logical_completion=logical.summary();pm.lock_wait=wait.summary();
        pm.lock_call=call.summary();pm.release=release.summary();
    }
    result.elapsed_ns=elapsed;
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
} // namespace
RunStats run_compact_record_workload(BTreeIndex& index,const WorkloadTrace& trace,const PolicyRunConfig& cfg) {
#define RUN(P) return cfg.record_latency ? (cfg.record_statistics ? run_record_impl<P,true,true>(index,trace,cfg) : run_record_impl<P,true,false>(index,trace,cfg)) : \
    (cfg.record_statistics ? run_record_impl<P,false,true>(index,trace,cfg) : run_record_impl<P,false,false>(index,trace,cfg))
    switch(cfg.wait_policy) {
        case WaitPolicy::NoWait: RUN(WaitPolicy::NoWait);
        case WaitPolicy::WaitDie: RUN(WaitPolicy::WaitDie);
        case WaitPolicy::WoundWait: RUN(WaitPolicy::WoundWait);
    }
#undef RUN
    throw std::invalid_argument("invalid record wait policy");
}
RunStats run_compact_key_workload(BTreeIndex& index,const WorkloadTrace& trace,const PolicyRunConfig& cfg) {
#define RUN_KEY(W,P) return cfg.key_latency ? (cfg.key_statistics ? run_record_impl<W,true,true,P>(index,trace,cfg) : run_record_impl<W,true,false,P>(index,trace,cfg)) : \
    (cfg.key_statistics ? run_record_impl<W,false,true,P>(index,trace,cfg) : run_record_impl<W,false,false,P>(index,trace,cfg))
#define CASE(P) case P: switch(cfg.wait_policy) { \
    case WaitPolicy::NoWait:RUN_KEY(WaitPolicy::NoWait,P); \
    case WaitPolicy::WaitDie:RUN_KEY(WaitPolicy::WaitDie,P); \
    case WaitPolicy::WoundWait:RUN_KEY(WaitPolicy::WoundWait,P); } break
    switch(cfg.protocol) {
        CASE(PolicyProtocol::NextKey);
        CASE(PolicyProtocol::Gap);
        CASE(PolicyProtocol::PriorKey);
        default:break;
    }
#undef CASE
#undef RUN_KEY
    throw std::invalid_argument("invalid compact key protocol/policy");
}
RunStats run_compact_exact_workload(BTreeIndex& index,const WorkloadTrace& trace,const PolicyRunConfig& cfg) {
#define RUN_E(P) return cfg.exact_latency ? (cfg.exact_statistics ? run_record_impl<P,true,true,PolicyProtocol::ExactRange>(index,trace,cfg) : run_record_impl<P,true,false,PolicyProtocol::ExactRange>(index,trace,cfg)) : \
    (cfg.exact_statistics ? run_record_impl<P,false,true,PolicyProtocol::ExactRange>(index,trace,cfg) : run_record_impl<P,false,false,PolicyProtocol::ExactRange>(index,trace,cfg))
    switch(cfg.wait_policy) {
        case WaitPolicy::NoWait:RUN_E(WaitPolicy::NoWait);
        case WaitPolicy::WaitDie:RUN_E(WaitPolicy::WaitDie);
        case WaitPolicy::WoundWait:RUN_E(WaitPolicy::WoundWait);
    }
#undef RUN_E
    throw std::invalid_argument("invalid exact-range wait policy");
}
RunStats run_compact_hierarchy_workload(BTreeIndex& index,const WorkloadTrace& trace,const PolicyRunConfig& cfg) {
#define RUN_H(P) return cfg.hierarchy_latency ? (cfg.hierarchy_statistics ? run_record_impl<P,true,true,PolicyProtocol::Hierarchy>(index,trace,cfg) : run_record_impl<P,true,false,PolicyProtocol::Hierarchy>(index,trace,cfg)) : \
    (cfg.hierarchy_statistics ? run_record_impl<P,false,true,PolicyProtocol::Hierarchy>(index,trace,cfg) : run_record_impl<P,false,false,PolicyProtocol::Hierarchy>(index,trace,cfg))
    switch(cfg.wait_policy) {
        case WaitPolicy::NoWait:RUN_H(WaitPolicy::NoWait);
        case WaitPolicy::WaitDie:RUN_H(WaitPolicy::WaitDie);
        case WaitPolicy::WoundWait:RUN_H(WaitPolicy::WoundWait);
    }
#undef RUN_H
    throw std::invalid_argument("invalid hierarchy wait policy");
}
} // namespace rrl
