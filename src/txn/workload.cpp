#include "txn/workload.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <random>
#include <thread>
namespace rrl { namespace {
struct Zipf { std::vector<double> c; explicit Zipf(size_t n,double theta):c(n){ double z=0; for(size_t i=1;i<=n;i++)z+=1.0/std::pow(double(i),theta); double s=0; for(size_t i=1;i<=n;i++){s+=(1.0/std::pow(double(i),theta))/z;c[i-1]=s;} if(!c.empty())c.back()=1; } idx_key_t get(std::mt19937_64& g)const{if(c.empty())return 0;double x=std::generate_canonical<double,53>(g);return idx_key_t(std::lower_bound(c.begin(),c.end(),x)-c.begin());}};
idx_key_t key(std::mt19937_64& g,const WorkloadConfig& c,const Zipf& z){if(!c.key_count)return 0;return c.zipf_theta>0?z.get(g):std::uniform_int_distribution<idx_key_t>(0,c.key_count-1)(g);}
void ycsb(TransactionTrace& t,const WorkloadConfig& c,std::mt19937_64& g,const Zipf& z,size_t n,size_t& mx){double rr=c.kind==WorkloadKind::YcsbA?.5:.95;std::bernoulli_distribution read(rr),ins(.05);for(size_t i=0;i<c.operations_per_transaction;i++){idx_key_t k=key(g,c,z);Operation o;o.value=(uint64_t(n)<<32)|i;if(c.kind==WorkloadKind::YcsbE&&!ins(g)){o.kind=OperationKind::RangeRead;o.key=k;o.end_key=std::min<idx_key_t>(c.key_count,k+c.range_length);}else if(c.kind==WorkloadKind::YcsbE){o.kind=OperationKind::Insert;o.key=c.key_count+n*c.operations_per_transaction+i;}else{o.kind=read(g)?OperationKind::Read:OperationKind::Update;o.key=k;}mx=std::max(mx,size_t(o.key));t.operations.push_back(o);}}
void payment(TransactionTrace& t,const WorkloadConfig& c,size_t n,size_t& mx){idx_key_t w=n%std::max<size_t>(1,c.key_count),d=1000000+w*10+n%10,u=2000000+w*30000+n%30000;t.operations={{OperationKind::Update,w,w,n+1},{OperationKind::Update,d,d,n+2},{OperationKind::Update,u,u,n+3}};mx=std::max(mx,size_t(u));}
void new_order(TransactionTrace& t,const WorkloadConfig& c,std::mt19937_64& g,size_t n,size_t& mx){idx_key_t w=n%std::max<size_t>(1,c.key_count),d=1000000+w*10+n%10,u=2000000+w*30000+n%30000;t.operations={{OperationKind::Read,w,w,n+1},{OperationKind::Read,u,u,n+2},{OperationKind::Update,d,d,n+3}};std::uniform_int_distribution<idx_key_t> item(0,std::max<size_t>(1,c.key_count)-1);for(size_t i=0;i<c.tpcc_items_per_order;i++){idx_key_t ik=3000000+item(g),sk=4000000+w*std::max<size_t>(1,c.key_count)+item(g);t.operations.push_back({OperationKind::Read,ik,ik,n+i+4});t.operations.push_back({OperationKind::Update,sk,sk,n+i+5});mx=std::max(mx,size_t(sk));}mx=std::max(mx,size_t(u));}
}
WorkloadKind WorkloadTrace::parse_kind(const std::string& in){std::string v;for(char x:in)v+=char(std::tolower((unsigned char)x));if(v=="a"||v=="ycsb-a")return WorkloadKind::YcsbA;if(v=="b"||v=="ycsb-b")return WorkloadKind::YcsbB;if(v=="e"||v=="ycsb-e")return WorkloadKind::YcsbE;if(v=="payment"||v=="tpcc-payment")return WorkloadKind::TpccPayment;if(v=="new-order"||v=="tpcc-new-order")return WorkloadKind::TpccNewOrder;return WorkloadKind::YcsbA;}
const char* WorkloadTrace::name(WorkloadKind k){switch(k){case WorkloadKind::YcsbA:return "ycsb-a";case WorkloadKind::YcsbB:return "ycsb-b";case WorkloadKind::YcsbE:return "ycsb-e";case WorkloadKind::TpccPayment:return "tpcc-payment";case WorkloadKind::TpccNewOrder:return "tpcc-new-order";}return "unknown";}
WorkloadTrace WorkloadTrace::generate(const WorkloadConfig& c){WorkloadTrace t;t.config_=c;std::mt19937_64 g(c.seed);Zipf z(c.key_count,c.zipf_theta);t.transactions_.reserve(c.transaction_count);for(size_t i=0;i<c.transaction_count;i++){TransactionTrace x;x.txn_id=i+1;if(c.kind==WorkloadKind::TpccPayment)payment(x,c,i,t.max_key_);else if(c.kind==WorkloadKind::TpccNewOrder)new_order(x,c,g,i,t.max_key_);else ycsb(x,c,g,z,i,t.max_key_);t.transactions_.push_back(std::move(x));}return t;}
double RunStats::throughput()const{return elapsed_ns?double(transactions)*1e9/double(elapsed_ns):0;}
RunStats run_workload(BTreeIndex& index,const WorkloadTrace& trace,size_t threads){if(!threads)threads=1;size_t seed_n=std::max(trace.config().key_count,trace.max_key()+1);std::vector<uint64_t> seed(seed_n);for(size_t k=0;k<trace.config().key_count;k++){seed[k]=k;index.insert(k,&seed[k]);}for(const auto& txn:trace.transactions())for(const auto& op:txn.operations)if(op.kind!=OperationKind::Insert&&op.kind!=OperationKind::Delete){seed[op.key]=op.key;void* existing=nullptr;if(!index.lookup(op.key,existing))index.insert(op.key,&seed[op.key]);}struct A{std::atomic<uint64_t> t{0},o{0},r{0},u{0},i{0},d{0},q{0},f{0},s{0},c{0};}a;auto begin=std::chrono::steady_clock::now();size_t wn=std::min(threads,std::max<size_t>(1,trace.transactions().size()));std::vector<std::thread> ws;for(size_t w=0;w<wn;w++)ws.emplace_back([&,w]{for(size_t j=w;j<trace.transactions().size();j+=wn){for(const Operation& op:trace.transactions()[j].operations){++a.o;switch(op.kind){case OperationKind::Read:{++a.r;void* p=nullptr;if(index.lookup(op.key,p)){++a.f;a.c.fetch_add(op.key^*static_cast<uint64_t*>(p));}break;}case OperationKind::Update:++a.u;index.insert(op.key,const_cast<uint64_t*>(&op.value));break;case OperationKind::Insert:++a.i;index.insert(op.key,const_cast<uint64_t*>(&op.value));break;case OperationKind::Delete:++a.d;index.erase(op.key);break;case OperationKind::RangeRead:{++a.q;auto out=index.scan_range(KeyRangeEnt::half_open(op.key,op.end_key));a.s+=out.size();a.f+=out.size();for(auto& v:out)a.c.fetch_add(v.key^*static_cast<uint64_t*>(v.value));break;}}}++a.t;}});for(auto& w:ws)w.join();auto elapsed=std::chrono::steady_clock::now()-begin;RunStats r;r.transactions=a.t;r.committed=a.t;r.operations=a.o;r.reads=a.r;r.updates=a.u;r.inserts=a.i;r.deletes=a.d;r.range_reads=a.q;r.rows_found=a.f;r.rows_scanned=a.s;r.checksum=a.c;r.elapsed_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();return r;}
}
namespace rrl {
RunStats run_record_2pl(BTreeIndex& index, LockManager& locks, const WorkloadTrace& trace, size_t threads) {
    if (!threads) threads = 1;
    const size_t seed_n = std::max(trace.config().key_count, trace.max_key() + 1);
    std::vector<uint64_t> seed(seed_n);
    for (size_t k = 0; k < trace.config().key_count; ++k) { seed[k] = k; index.insert(k, &seed[k]); }
    for (const auto& txn : trace.transactions()) for (const auto& op : txn.operations)
        if (op.kind != OperationKind::Insert && op.kind != OperationKind::Delete) {
            seed[op.key] = op.key; void* existing = nullptr;
            if (!index.lookup(op.key, existing)) index.insert(op.key, &seed[op.key]);
        }
    struct A { std::atomic<uint64_t> tx{0}, committed{0}, aborted{0}, ops{0}, reads{0}, updates{0}, inserts{0}, deletes{0}, ranges{0}, found{0}, scanned{0}, acquires{0}, conflicts{0}, checksum{0}; } a;
    const auto begin = std::chrono::steady_clock::now();
    const size_t worker_count = std::min(threads, std::max<size_t>(1, trace.transactions().size()));
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < worker_count; ++worker) workers.emplace_back([&, worker] {
        for (size_t i = worker; i < trace.transactions().size(); i += worker_count) {
            const auto& txn = trace.transactions()[i]; bool ok = true; std::vector<const Operation*> writes;
            for (const Operation& op : txn.operations) {
                ++a.ops;
                if (op.kind == OperationKind::RangeRead) {
                    ++a.ranges; const auto rows = index.scan_range(KeyRangeEnt::half_open(op.key, op.end_key));
                    a.scanned += rows.size(); a.found += rows.size();
                    for (const auto& row : rows) {
                        ++a.acquires;
                        if (locks.lock(txn.txn_id, LockMode::S, KeyRangeEnt::point(row.key), LockKind::Record) != RCOK) { ++a.conflicts; ok = false; break; }
                        a.checksum += row.key ^ *static_cast<uint64_t*>(row.value);
                    }
                } else {
                    const LockMode mode = (op.kind == OperationKind::Read) ? LockMode::S : LockMode::X;
                    ++a.acquires;
                    if (locks.lock(txn.txn_id, mode, KeyRangeEnt::point(op.key), LockKind::Record) != RCOK) { ++a.conflicts; ok = false; break; }
                    if (op.kind == OperationKind::Read) {
                        ++a.reads; void* value = nullptr;
                        if (index.lookup(op.key, value)) { ++a.found; a.checksum += op.key ^ *static_cast<uint64_t*>(value); }
                    } else if (op.kind == OperationKind::Update) { ++a.updates; writes.push_back(&op); }
                    else if (op.kind == OperationKind::Insert) { ++a.inserts; writes.push_back(&op); }
                    else if (op.kind == OperationKind::Delete) { ++a.deletes; writes.push_back(&op); }
                }
                if (!ok) break;
            }
            if (ok) {
                for (const Operation* op : writes) {
                    if (op->kind == OperationKind::Delete) index.erase(op->key);
                    else index.insert(op->key, const_cast<uint64_t*>(&op->value));
                }
                ++a.committed;
            } else ++a.aborted;
            if (locks.get_lock_count(txn.txn_id) > 0) locks.unlock_all(txn.txn_id);
            ++a.tx;
        }
    });
    for (auto& worker : workers) worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    RunStats result; result.transactions=a.tx; result.committed=a.committed; result.aborted=a.aborted; result.operations=a.ops;
    result.reads=a.reads; result.updates=a.updates; result.inserts=a.inserts; result.deletes=a.deletes; result.range_reads=a.ranges;
    result.rows_found=a.found; result.rows_scanned=a.scanned; result.lock_acquires=a.acquires; result.lock_conflicts=a.conflicts; result.checksum=a.checksum;
    result.elapsed_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count(); return result;
}}

namespace rrl {
RunStats run_gap_lock(BTreeIndex& index, LockManager& locks, const WorkloadTrace& trace, size_t threads) {
    if (!threads) threads = 1;
    const size_t seed_n = std::max(trace.config().key_count, trace.max_key() + 1);
    std::vector<uint64_t> seed(seed_n);
    for (size_t k = 0; k < trace.config().key_count; ++k) { seed[k] = k; index.insert(k, &seed[k]); }
    for (const auto& txn : trace.transactions()) for (const auto& op : txn.operations)
        if (op.kind != OperationKind::Insert && op.kind != OperationKind::Delete) {
            seed[op.key] = op.key; void* existing = nullptr;
            if (!index.lookup(op.key, existing)) index.insert(op.key, &seed[op.key]);
        }
    struct A { std::atomic<uint64_t> tx{0}, committed{0}, aborted{0}, ops{0}, reads{0}, updates{0}, inserts{0}, deletes{0}, ranges{0}, found{0}, scanned{0}, acquires{0}, conflicts{0}, checksum{0}; } a;
    const auto begin = std::chrono::steady_clock::now();
    const size_t worker_count = std::min(threads, std::max<size_t>(1, trace.transactions().size()));
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < worker_count; ++worker) workers.emplace_back([&, worker] {
        auto acquire = [&](txnid_t txn, LockMode mode, const KeyRangeEnt& range, LockKind kind) {
            ++a.acquires;
            if (locks.lock(txn, mode, range, kind) != RCOK) { ++a.conflicts; return false; }
            return true;
        };
        for (size_t i = worker; i < trace.transactions().size(); i += worker_count) {
            const auto& txn = trace.transactions()[i]; bool ok = true; std::vector<const Operation*> writes;
            for (const Operation& op : txn.operations) {
                ++a.ops;
                if (op.kind == OperationKind::RangeRead) {
                    ++a.ranges;
                    const auto rows = index.scan_range(KeyRangeEnt::half_open(op.key, op.end_key));
                    a.scanned += rows.size(); a.found += rows.size();
                    if (!acquire(txn.txn_id, LockMode::S, KeyRangeEnt::open(op.key, op.end_key), LockKind::Gap) ||
                        !acquire(txn.txn_id, LockMode::S, KeyRangeEnt::point(op.key), LockKind::Gap) ||
                        !acquire(txn.txn_id, LockMode::S, KeyRangeEnt::point(op.end_key), LockKind::Gap)) { ok = false; break; }
                    for (const auto& row : rows) {
                        if (!acquire(txn.txn_id, LockMode::S, KeyRangeEnt::point(row.key), LockKind::Record)) { ok = false; break; }
                        a.checksum += row.key ^ *static_cast<uint64_t*>(row.value);
                    }
                } else if (op.kind == OperationKind::Read) {
                    if (!acquire(txn.txn_id, LockMode::S, KeyRangeEnt::point(op.key), LockKind::Record)) { ok = false; break; }
                    ++a.reads; void* value = nullptr;
                    if (index.lookup(op.key, value)) { ++a.found; a.checksum += op.key ^ *static_cast<uint64_t*>(value); }
                } else {
                    const LockKind kind = op.kind == OperationKind::Insert ? LockKind::Gap : LockKind::Record;
                    if (!acquire(txn.txn_id, LockMode::X, KeyRangeEnt::point(op.key), kind)) { ok = false; break; }
                    if (op.kind == OperationKind::Update) { ++a.updates; writes.push_back(&op); }
                    else if (op.kind == OperationKind::Insert) { ++a.inserts; writes.push_back(&op); }
                    else if (op.kind == OperationKind::Delete) { ++a.deletes; writes.push_back(&op); }
                }
            }
            if (ok) { for (const Operation* op : writes) { if (op->kind == OperationKind::Delete) index.erase(op->key); else index.insert(op->key, const_cast<uint64_t*>(&op->value)); } ++a.committed; }
            else ++a.aborted;
            if (locks.get_lock_count(txn.txn_id) > 0) locks.unlock_all(txn.txn_id);
            ++a.tx;
        }
    });
    for (auto& worker : workers) worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    RunStats result; result.transactions=a.tx; result.committed=a.committed; result.aborted=a.aborted; result.operations=a.ops;
    result.reads=a.reads; result.updates=a.updates; result.inserts=a.inserts; result.deletes=a.deletes; result.range_reads=a.ranges;
    result.rows_found=a.found; result.rows_scanned=a.scanned; result.lock_acquires=a.acquires; result.lock_conflicts=a.conflicts; result.checksum=a.checksum;
    result.elapsed_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count(); return result;
}
}