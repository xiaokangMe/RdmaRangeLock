#include "lock/backend_lifecycle.h"
#include "txn/workload.h"
#include "txn/policy_workload.h"
#include "txn/coroutine.h"
#include "common/execution_context.h"
#include "lock/native_policy.h"
#include "lock/object_lock_manager.h"
#include "lock/range_batch.h"
#include "hierarchical_plan.h"
#include "leaf_hierarchy.h"
#include "key_range_plan.h"
#include "bucket_runtime.h"
#include "dag_hierarchy.h"
#include "precision_plan.h"
#include "index/griffin.h"
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

}
static RunStats run_policy_workload_impl(BTreeIndex& index,IRangeLockManager& physical,const WorkloadTrace& trace,const PolicyRunConfig& cfg,std::size_t worker_threads) {
    if((cfg.protocol==PolicyProtocol::None && cfg.wait_policy!=WaitPolicy::NoWait) ||
       !worker_threads || !cfg.max_attempts || !cfg.wait_timeout_ms || cfg.wait_timeout_ms>600000)throw std::invalid_argument("invalid retry/wait bound");
    if(dynamic_cast<BTreeGriffin*>(&index) && cfg.protocol!=PolicyProtocol::Griffin)
        throw std::invalid_argument("Griffin hybrid index requires Griffin phantom and record protocol");
    const bool conversions=cfg.hierarchy_escalation_threshold || cfg.hierarchy_deescalate_after;
    auto* converter=dynamic_cast<ILockModeConversion*>(&physical);
    if(conversions && (cfg.protocol!=PolicyProtocol::Hierarchy || cfg.leaf_hierarchy || cfg.separator_hierarchy || cfg.dag_hierarchy ||
       !converter || (!cfg.coordinator_host.empty() && !cfg.txn_state_port)))
        throw std::invalid_argument("granularity conversion requires fixed buckets and local/native RDMA state");
    if(cfg.dag_hierarchy) {
        detail::check_dag_geometry(trace.config().hierarchy_bucket_width,cfg.hierarchy_index_paths,cfg.hierarchy_read_path);
        auto* object=dynamic_cast<ObjectLockManager*>(&physical);
        if(cfg.protocol!=PolicyProtocol::Hierarchy || cfg.leaf_hierarchy || cfg.deneva_insert_behavior ||
           !object || !object->matches_dag(trace.config().hierarchy_bucket_width,cfg.hierarchy_index_paths) ||
           (!cfg.coordinator_host.empty() && !cfg.txn_state_port))
            throw std::invalid_argument("DAG requires exact objects and local/native RDMA state");
    }
    std::unordered_map<idx_key_t,std::uint64_t> seed;
    if(!index.preloaded() && !seed_index(index,trace,seed)){RunStats s;s.index_errors=1;return s;}
    const auto total=trace.transactions().size();
    if(!total)return {};
    if(!cfg.coroutines || cfg.coroutines>4096 || worker_threads>4096/cfg.coroutines ||
       cfg.coroutine_stack_bytes<65536 || cfg.coroutine_stack_bytes>16777216 || cfg.warmup_seconds>600000)
        throw std::invalid_argument("runtime geometry bounds");
    if(!cfg.use_coroutines && cfg.coroutines!=1)
        throw std::invalid_argument("threads execution requires coroutines=1");
    if(cfg.coroutines>1 && !cfg.txn_state_port && !cfg.coordinator_host.empty())
        throw std::invalid_argument("TCP compatibility coordinator cannot block a coroutine worker");
    const auto count=std::min(worker_threads*cfg.coroutines,total);
    const auto os_workers=std::min(worker_threads,count);
    const bool warmup=cfg.warmup_seconds!=0;
    LocalNativePolicyState local_native_state(std::max<std::size_t>(count, 1));
    std::vector<std::unique_ptr<PolicyClient>> clients;
    std::vector<std::unique_ptr<NativePolicyClient>> native;
    std::vector<std::unique_ptr<LocalNativePolicyClient>> local_native;
    std::vector<Worker> parts(count);
    std::vector<std::vector<detail::HierarchyRequest>> plans;
    if(cfg.protocol==PolicyProtocol::Hierarchy && !cfg.leaf_hierarchy && !cfg.separator_hierarchy && !conversions)for(const auto& t:trace.transactions())
        plans.push_back(cfg.dag_hierarchy?detail::dag_plan(t,trace.config().hierarchy_bucket_width,cfg.hierarchy_index_paths,cfg.hierarchy_read_path):
            detail::hierarchy_plan(t,trace.config().hierarchy_bucket_width));
    for(std::size_t w=0;w<count;++w) {
        if(cfg.protocol==PolicyProtocol::None) {
            clients.emplace_back(nullptr);
            native.emplace_back(nullptr);
            local_native.emplace_back(nullptr);
        } else if(cfg.txn_state_port) {
            native.emplace_back(new NativePolicyClient(cfg.coordinator_host,cfg.txn_state_port,physical));
            local_native.emplace_back(nullptr);
            clients.emplace_back(nullptr);
        } else if(cfg.coordinator_host.empty()) {
            local_native.emplace_back(new LocalNativePolicyClient(local_native_state,physical));
            native.emplace_back(nullptr);
            clients.emplace_back(nullptr);
        } else {
            local_native.emplace_back(nullptr);
            native.emplace_back(nullptr);
            clients.emplace_back(new PolicyClient(cfg.coordinator_host,cfg.coordinator_port,cfg.wait_timeout_ms+5000));
        }
    }
    if(cfg.deneva_insert_behavior && (!cfg.leaf_hierarchy || cfg.separator_hierarchy || cfg.protocol!=PolicyProtocol::Hierarchy || !cfg.txn_state_port))
        throw std::invalid_argument("Deneva insert behavior requires native RDMA leaf hierarchy");
    std::vector<TransactionTrace> operation_txns(count);
    std::vector<std::vector<IndexMutation>> immediate_writes(count);
    for(std::size_t w=0;w<count;++w){operation_txns[w].operations.resize(1);immediate_writes[w].reserve(1);}
    std::vector<std::vector<IndexMutation>> writes(count);
    std::vector<std::vector<KeyValue>> scans(count),rechecks(count);
    std::vector<std::vector<KeyRangeEnt>> record_requests(count);
    const bool classic=cfg.protocol==PolicyProtocol::Gap || cfg.protocol==PolicyProtocol::NextKey || cfg.protocol==PolicyProtocol::PriorKey;
    std::vector<detail::KeyObservation> key_before(count),key_after(count);
    std::vector<std::vector<detail::KeyRequest>> key_plans(count);
    const auto scan_capacity=trace.config().range_length;
    for(auto& v:scans)v.reserve(scan_capacity);
    for(auto& v:rechecks)v.reserve(scan_capacity);
    for(auto& v:record_requests)v.reserve(scan_capacity+2);
    for(std::size_t w=0;w<count;++w) {
        key_before[w].keys.reserve(scan_capacity);key_after[w].keys.reserve(scan_capacity);
        key_plans[w].reserve(2*scan_capacity+8);
    }
    std::size_t maxops=0;for(const auto& t:trace.transactions())maxops=std::max(maxops,t.operations.size());
    for(auto& v:writes)v.reserve(maxops);
    std::vector<std::vector<std::vector<detail::HierarchyRequest>>> operation_plans;
    std::vector<detail::RuntimeBuckets> runtime_buckets(count);
    if(conversions) {
        std::size_t bucket_capacity=0;
        for(const auto& txn:trace.transactions()) {
            const auto full=detail::hierarchy_plan(txn,trace.config().hierarchy_bucket_width);
            bucket_capacity=std::max(bucket_capacity,static_cast<std::size_t>(std::count_if(full.begin(),full.end(),[](const auto& r){return r.phase==1;})));
            std::vector<std::vector<detail::HierarchyRequest>> ops;
            for(const auto& op:txn.operations){TransactionTrace one;one.operations.push_back(op);ops.push_back(detail::hierarchy_plan(one,trace.config().hierarchy_bucket_width));}
            operation_plans.push_back(std::move(ops));
        }
        for(auto& runtime:runtime_buckets)runtime.reserve(bucket_capacity,maxops);
    }
    std::vector<std::vector<IndexNodeSpan>> leaf_scratch(count);
    std::vector<std::vector<detail::HierarchyRequest>> leaf_plans(count),leaf_checks(count);
    if(cfg.leaf_hierarchy || cfg.separator_hierarchy)for(std::size_t w=0;w<count;++w) {
        if(scan_capacity>SIZE_MAX/64-2 || (maxops && 64*(scan_capacity+2)>SIZE_MAX/maxops))throw std::invalid_argument("hierarchy workspace overflow");
        const auto capacity=std::min<std::size_t>(cfg.lock_budget+1,maxops*64*(scan_capacity+2)+1);
        leaf_scratch[w].reserve(64*(scan_capacity+2));
        leaf_plans[w].reserve(capacity);leaf_checks[w].reserve(capacity);
    }
    std::atomic<bool> stopped{false};
    Clock::time_point start,deadline;
    bool timed=warmup || trace.config().duration_seconds!=0;
    std::mutex start_mutex;std::condition_variable start_cv;
    std::size_t ready=0;
    std::atomic<unsigned> phase{0};
    std::vector<std::unique_ptr<CoroutineScheduler>> schedulers;
    if(cfg.use_coroutines)for(std::size_t w=0;w<os_workers;++w) {
        const auto routines=(count+os_workers-1-w)/os_workers;
        schedulers.emplace_back(new CoroutineScheduler(routines,cfg.coroutine_stack_bytes));
    }
    std::vector<std::thread> threads;
    auto work=[&](std::size_t worker) {
        auto& p=parts[worker];auto& s=p.stats;auto& m=s.policy;
        auto* nc=cfg.txn_state_port && cfg.protocol!=PolicyProtocol::None ? native[worker].get() : nullptr;
        auto* lc=!cfg.txn_state_port && cfg.coordinator_host.empty() && cfg.protocol!=PolicyProtocol::None ? local_native[worker].get() : nullptr;
        auto begin=[&](WaitPolicy policy,std::uint64_t age,std::uint64_t budget) {
            if(cfg.protocol==PolicyProtocol::None)return PolicyReply{TxnResult::Ok,worker+1,0,worker+1};
            return nc ? nc->begin(policy,age,budget) : lc ? lc->begin(policy,age,budget) : clients[worker]->begin(policy,age,budget);
        };
        auto lock=[&](txnid_t id,LockMode mode,const KeyRangeEnt& r,LockKind kind) {
            return nc ? nc->acquire(id,mode,r,kind) : lc ? lc->acquire(id,mode,r,kind) : clients[worker]->acquire(id,mode,r,kind);
        };
        auto check=[&](txnid_t id){return nc ? nc->check(id) : lc ? lc->check(id) : clients[worker]->check(id);};
        auto prepare=[&](txnid_t id){if(cfg.protocol==PolicyProtocol::None)return PolicyReply{TxnResult::Ok,id,0};return nc ? nc->prepare_commit(id) : lc ? lc->prepare_commit(id) : clients[worker]->prepare_commit(id);};
        auto end=[&](txnid_t id){if(cfg.protocol==PolicyProtocol::None)return PolicyReply{TxnResult::Ok,id,0};return nc ? nc->end(id) : lc ? lc->end(id) : clients[worker]->end(id);};
        auto arrive=[&](unsigned target) {
            {std::lock_guard<std::mutex> guard(start_mutex);++ready;start_cv.notify_all();}
            while(phase.load(std::memory_order_acquire)<target)cooperative_yield();
        };
        auto& pending=writes[worker];
        bool active_attempt=false,arrived=false;
        try {
        index.prepare_thread(worker,scan_capacity);index.prepare_write_capacity(maxops);
        if(auto* lifecycle=dynamic_cast<ILockBackendLifecycle*>(&physical))lifecycle->prepare_lock_thread(worker);
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
                bool touched=false,pending_stale=false;
                std::uint64_t early_in_attempt=0;
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
                    else if(lc)touched=lc->has_physical_locks();
                    if(nc && nc->last_lock_result()==AbortCapacity)++s.lock_capacity_aborts;
                    if(nc && nc->last_lock_result()==AbortGate)++s.lock_gate_aborts;
                    if(r.wait_ns)p.wait.add(r.wait_ns);
                    outcome=r.result;
                    if(outcome!=TxnResult::Ok){p.call.add(ns(Clock::now()-began));return false;}
                    touched=true;
                    const auto rc=(nc || lc) ? RCOK : physical.lock(id,mode,range,kind);
                    p.call.add(ns(Clock::now()-began));
                    // Metadata grant excludes every coordinated conflict.
                    // A foreign/legacy client or physical-table failure is a
                    // hard error, never an unbounded client conflict retry.
                    if(rc!=RCOK){outcome=TxnResult::Error;return false;}
                    return true;
                };
                auto acquire_batch=[&](LockMode mode,const std::vector<KeyRangeEnt>& ranges,LockKind kind) {
                    if(ranges.empty())return true;
                    if(Clock::now()>=deadline){outcome=TxnResult::Deadline;return false;}
                    const auto began=Clock::now();
                    if(nc || lc) {
                        s.lock_acquires+=ranges.size();
                        const auto r=nc ? nc->acquire_batch(id,mode,ranges,kind) : lc->acquire_batch(id,mode,ranges,kind);
                        if(nc)touched=nc->has_physical_locks();
                        else if(lc)touched=lc->has_physical_locks();
                        if(nc && nc->last_lock_result()==AbortCapacity)++s.lock_capacity_aborts;
                        if(nc && nc->last_lock_result()==AbortGate)++s.lock_gate_aborts;
                        if(r.wait_ns)p.wait.add(r.wait_ns);
                        outcome=r.result;p.call.add(ns(Clock::now()-began));
                        return outcome==TxnResult::Ok;
                    }
                    // Legacy TCP coordination has no batch RPC. Keep its
                    // established per-range path and accounting.
                    for(const auto& range:ranges)if(!acquire(mode,range,kind))return false;
                    return true;
                };
                auto acquire_key_plan=[&]() {
                    auto& plan=key_plans[worker];auto& ranges=record_requests[worker];
                    for(std::size_t first=0;first<plan.size();) {
                        const auto mode=plan[first].mode;const auto kind=plan[first].kind;
                        ranges.clear();std::size_t last=first;
                        while(last<plan.size() && plan[last].mode==mode && plan[last].kind==kind)
                            ranges.push_back(plan[last++].range);
                        if(!acquire_batch(mode,ranges,kind))return false;
                        first=last;
                    }
                    return true;
                };
                if(conversions)runtime_buckets[worker].reset();
                if(cfg.protocol==PolicyProtocol::Hierarchy && !conversions) {
                    if((cfg.leaf_hierarchy || cfg.separator_hierarchy) && !cfg.deneva_insert_behavior) {
                        if(cfg.separator_hierarchy ? !detail::separator_plan(index,txn,leaf_scratch[worker],leaf_plans[worker]) :
                            !detail::leaf_plan(index,txn,leaf_scratch[worker],leaf_plans[worker]))outcome=TxnResult::Error;
                        else {
                            for(const auto& r:leaf_plans[worker])if(!acquire(r.mode,r.range,r.kind))break;
                            if(outcome==TxnResult::Ok) {
                                if(cfg.separator_hierarchy ? !detail::separator_plan(index,txn,leaf_scratch[worker],leaf_checks[worker]) :
                                    !detail::leaf_plan(index,txn,leaf_scratch[worker],leaf_checks[worker]))outcome=TxnResult::Error;
                                else if(cfg.separator_hierarchy ? !detail::covers_separator_plan(leaf_plans[worker],leaf_checks[worker]) :
                                    !detail::covers_hierarchy_plan(leaf_plans[worker],leaf_checks[worker]))outcome=TxnResult::Conflict;
                            }
                        }
                    }else if(!cfg.leaf_hierarchy && !cfg.separator_hierarchy)for(const auto& r:plans[i%total])if(!acquire(r.mode,r.range,r.kind))break;
                }
                std::size_t operation_index=0;
                for(const auto& op:txn.operations) {
                    if(outcome!=TxnResult::Ok)break;
                    if(Clock::now()>=deadline){outcome=TxnResult::Deadline;break;}
                    if(cfg.wait_policy==WaitPolicy::WoundWait) {
                        outcome=check(id).result;
                        if(outcome!=TxnResult::Ok)break;
                    }
                    if(conversions && !runtime_buckets[worker].acquire_operation(op,operation_plans[i%total][operation_index],acquire))break;
                    ++operation_index;
                    if(cfg.deneva_insert_behavior) {
                        auto& one=operation_txns[worker];one.operations[0]=op;
                        if(!detail::leaf_plan(index,one,leaf_scratch[worker],leaf_plans[worker],true)) {outcome=TxnResult::Error;break;}
                        for(const auto& r:leaf_plans[worker])if(!acquire(r.mode,r.range,r.kind))break;
                        if(outcome!=TxnResult::Ok)break;
                        if(!detail::leaf_plan(index,one,leaf_scratch[worker],leaf_checks[worker],true)){outcome=TxnResult::Error;break;}
                        if(!detail::covers_hierarchy_plan(leaf_plans[worker],leaf_checks[worker])){outcome=TxnResult::Conflict;break;}
                    }
                    ++s.operations;
                    if(op.kind==OperationKind::RangeRead) {
                        ++s.range_reads;
                        if(op.end_key<=op.key)continue;
                        const auto range=KeyRangeEnt::half_open(op.key,op.end_key);
                        // Eswaran1976 section 3, single-key-interval language.
                        // ExactRange is an equivalent compatibility tag.
                        if((cfg.protocol==PolicyProtocol::Predicate && !acquire(LockMode::S,range,LockKind::Predicate)) ||
                           (cfg.protocol==PolicyProtocol::ExactRange && !acquire(LockMode::S,range,LockKind::ExactRange))) break;
                        if((cfg.protocol==PolicyProtocol::Precision || cfg.protocol==PolicyProtocol::Griffin) &&
                           !acquire(LockMode::S,range,cfg.protocol==PolicyProtocol::Griffin?LockKind::GriffinPredicate:LockKind::PrecisionPredicate))break;
                        if(classic) {
                            auto& before=key_before[worker];auto& after=key_after[worker];
                            if(!detail::observe_keys(index,op,before)){outcome=TxnResult::Conflict;break;}
                            // Candidate values are never consumed. The second
                            // scan reads only after the complete protection set.
                            auto& rows=scans[worker];index.scan_range_into(range,rows);s.rows_scanned+=rows.size();
                            detail::key_plan(op,before,cfg.protocol,key_plans[worker]);
                            if(!acquire_key_plan())break;
                            if(!detail::observe_keys(index,op,after) ||
                               !detail::same_observation(before,after,true)){outcome=TxnResult::Conflict;break;}
                            auto& verified=rechecks[worker];index.scan_range_into(range,verified);s.rows_rechecked+=verified.size();
                            for(const auto& row:verified)account_value(row.key,static_cast<const std::uint64_t*>(row.value));
                            account_inserted(range,verified);
                        } else if(cfg.protocol==PolicyProtocol::Griffin) {
                            auto& rows=scans[worker];
                            try {index.scan_range_into(range,rows);}
                            catch(const IndexSyncPending&) {++m.index_pending_aborts;pending_stale=true;outcome=TxnResult::Conflict;break;}
                            s.rows_scanned+=rows.size();
                            auto& requests=record_requests[worker];requests.clear();
                            for(const auto& row:rows)requests.push_back(KeyRangeEnt::point(row.key));
                            if(!acquire_batch(LockMode::S,requests,LockKind::Record))break;
                            for(const auto& row:rows) {
                                void* value=nullptr;
                                if(own_write(row.key))account_value(row.key,nullptr);
                                else if(index.lookup(row.key,value))account_value(row.key,static_cast<const std::uint64_t*>(value));
                                else {outcome=TxnResult::Error;break;}
                            }
                            if(outcome==TxnResult::Ok)account_inserted(range,rows);
                        } else if(cfg.protocol==PolicyProtocol::Record) {
                            auto& rows=scans[worker];index.scan_range_into(range,rows);s.rows_scanned+=rows.size();
                            auto& requests=record_requests[worker];requests.clear();
                            for(const auto& row:rows)requests.push_back(KeyRangeEnt::point(row.key));
                            if(!acquire_batch(LockMode::S,requests,LockKind::Record))break;
                            // Re-read values only after record S locks are granted.
                            // One scan replaces a fresh root descent for every row.
                            auto& verified=rechecks[worker];index.scan_range_into(range,verified);
                            s.rows_rechecked+=verified.size();
                            std::size_t n=0;
                            for(const auto& row:rows) {
                                while(n<verified.size() && verified[n].key<row.key)++n;
                                if(n==verified.size() || verified[n].key!=row.key) {
                                    outcome=TxnResult::Conflict;break;
                                }
                                account_value(row.key,static_cast<const std::uint64_t*>(verified[n].value));
                            }
                            // Record-only scans do not lock gaps. Newly inserted
                            // keys are deliberately not consumed without a lock.
                            if(outcome==TxnResult::Ok)account_inserted(range,verified);
                        } else {
                            auto& rows=scans[worker];index.scan_range_into(range,rows);s.rows_scanned+=rows.size();
                            for(const auto& row:rows)account_value(row.key,static_cast<const std::uint64_t*>(row.value));
                            account_inserted(range,rows);
                        }
                    } else {
                        const bool read=op.kind==OperationKind::Read;
                        const auto kind=cfg.protocol==PolicyProtocol::Predicate ? LockKind::Predicate :
                            cfg.protocol==PolicyProtocol::ExactRange ? LockKind::ExactRange :
                            cfg.protocol==PolicyProtocol::NextKey ? LockKind::NextKey :
                            cfg.protocol==PolicyProtocol::Gap && op.kind==OperationKind::Insert ? LockKind::Gap : LockKind::Record;
                        if(classic) {
                            auto& before=key_before[worker];auto& after=key_after[worker];
                            if(!detail::observe_keys(index,op,before)){outcome=TxnResult::Conflict;break;}
                            detail::key_plan(op,before,cfg.protocol,key_plans[worker]);
                            if(!acquire_key_plan())break;
                            if(!detail::observe_keys(index,op,after) ||
                               !detail::same_observation(before,after,false)){outcome=TxnResult::Conflict;break;}
                        }
                        if(cfg.protocol==PolicyProtocol::Precision) {
                            if(!detail::precision_point(op,acquire))break;
                        } else if(cfg.protocol==PolicyProtocol::Griffin) {
                            // Griffin is phantom avoidance, not value isolation.
                            // The paper explicitly measured only single-operation
                            // index transactions (§V-A). Add record 2PL for our
                            // common multi-operation transaction harness.
                            if(!acquire(read?LockMode::S:LockMode::X,KeyRangeEnt::point(op.key),LockKind::Record))break;
                            bool membership=!read && op.kind!=OperationKind::Update;
                            if(op.kind==OperationKind::Update) {
                                void* old=nullptr;
                                const auto* own=own_write(op.key);
                                membership=own?own->remove:!index.lookup(op.key,old);
                            }
                            if(membership && !acquire(LockMode::X,KeyRangeEnt::point(op.key),LockKind::GriffinUpdate))break;
                        } else if(!classic && cfg.protocol!=PolicyProtocol::Hierarchy && cfg.protocol!=PolicyProtocol::None &&
                                  !acquire(read?LockMode::S:LockMode::X,KeyRangeEnt::point(op.key),kind))break;
                        if(read) {
                            ++s.reads;void* value=nullptr;
                            if(own_write(op.key))account_value(op.key,nullptr);
                            else if(index.lookup(op.key,value))account_value(op.key,static_cast<const std::uint64_t*>(value));
                        } else {
                            if(op.kind==OperationKind::Update)++s.updates;
                            else if(op.kind==OperationKind::Insert)++s.inserts;
                            else ++s.deletes;
                            if(cfg.deneva_insert_behavior && op.kind==OperationKind::Insert) {
                                auto& nodes=leaf_scratch[worker];
                                if(!index.hierarchy_cover_into(KeyRangeEnt::point(op.key),nodes) || nodes.empty()){outcome=TxnResult::Error;break;}
                                const auto leaf=nodes.back();
                                if(leaf.keys>=leaf.max_keys-1) {
                                    // Source txn.cpp:5064..5090: parent CAS,
                                    // release, Abort in BOTH parent-full and
                                    // parent-not-full branches; split unreachable.
                                    const auto parent=nodes.size()>1?nodes[nodes.size()-2].id:0;
                                    ++s.lock_acquires;const auto parent_start=Clock::now();
                                    const auto rc=physical.lock(id,LockMode::X,KeyRangeEnt::point(parent),LockKind::Node);
                                    p.call.add(ns(Clock::now()-parent_start));
                                    if(rc==ERROR){outcome=TxnResult::Error;break;}
                                    if(rc==RCOK && physical.unlock(id,KeyRangeEnt::point(parent),LockKind::Node)!=RCOK){outcome=TxnResult::Error;break;}
                                    ++m.deneva_split_aborts;outcome=TxnResult::Conflict;break;
                                }
                                auto& immediate=immediate_writes[worker];immediate.clear();immediate.push_back({op.key,&op.value,false});
                                const auto rc=index.apply_batch(immediate);
                                if(rc==AbortCapacity){++s.lock_capacity_aborts;outcome=TxnResult::Conflict;break;}
                                if(rc!=RCOK || !index.healthy())throw std::runtime_error("uncertain Deneva early insert; retain locks");
                                ++early_in_attempt;++m.deneva_early_inserts;
                                // Preserve operation order against earlier
                                // private writes to this same key.
                                pending.erase(std::remove_if(pending.begin(),pending.end(),[&](const IndexMutation& value){return value.key==op.key;}),pending.end());
                                if(physical.unlock(id,KeyRangeEnt::point(leaf.id),LockKind::Node)!=RCOK){outcome=TxnResult::Error;break;}
                                ++m.deneva_early_unlocks;
                            }else pending.push_back({op.key,&op.value,op.kind==OperationKind::Delete});
                        }
                    }
                    if(conversions && outcome==TxnResult::Ok) {
                        auto convert=[&](LockMode mode,const KeyRangeEnt& range) {
                            outcome=check(id).result;
                            if(outcome!=TxnResult::Ok)return ERROR;
                            const auto rc=converter->convert_lock(id,mode,range,LockKind::Node);
                            if(rc!=RCOK && rc!=AbortConflict && rc!=AbortCapacity && rc!=AbortGate)outcome=TxnResult::Error;
                            else if(rc==RCOK)outcome=check(id).result;
                            return outcome==TxnResult::Ok?rc:ERROR;
                        };
                        auto release_fine=[&](const detail::HierarchyRequest& r) {
                            if(physical.unlock(id,r.range,r.kind)==RCOK)return true;
                            ++s.lock_release_failures;outcome=TxnResult::Error;return false;
                        };
                        if(!runtime_buckets[worker].finish_operation(cfg,m,acquire,convert,release_fine))break;
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
                if(early_in_attempt)++m.deneva_partial_write_aborts;
                ++s.aborted;
                if(outcome==TxnResult::Conflict && !pending_stale)++s.lock_conflicts;
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
RunStats run_policy_workload(BTreeIndex& index,IRangeLockManager& locks,const WorkloadTrace& trace,const PolicyRunConfig& cfg) {
    return run_policy_workload_impl(index,locks,trace,cfg,trace.config().threads);
}
namespace detail {
RunStats run_classic_workload(BTreeIndex& index,IRangeLockManager& locks,const WorkloadTrace& trace,
                             std::size_t threads,PolicyProtocol protocol) {
    PolicyRunConfig cfg;cfg.protocol=protocol;cfg.max_attempts=1;cfg.use_coroutines=false;
    return run_policy_workload_impl(index,locks,trace,cfg,std::max<std::size_t>(1,threads));
}
}
} // namespace rrl
