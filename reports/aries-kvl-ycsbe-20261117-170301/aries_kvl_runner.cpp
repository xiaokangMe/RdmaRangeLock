#include "index/aries_kvl.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace rrl::aries;
using Clock = std::chrono::steady_clock;
struct Config {
    std::uint64_t keys=2097152, trace_txns=5000, ops=10, width=32, seed=8;
    unsigned threads=32, order=128, timeout_ms=100, max_attempts=1000;
    double theta=0, warmup=10, duration=10;
};
struct Op { bool insert; std::uint64_t key,end,value; };
using Trace=std::vector<std::vector<Op>>;
Trace generate(const Config& c) {
    std::mt19937_64 rng(c.seed);
    std::uniform_int_distribution<std::uint64_t> uniform(0,c.keys-1);
    std::bernoulli_distribution insert(.05);
    std::vector<double> cdf;
    if(c.theta>0) {
        cdf.resize(c.keys); double sum=0;
        for(std::uint64_t i=0;i<c.keys;++i)sum+=1/std::pow(double(i+1),c.theta);
        double acc=0;
        for(std::uint64_t i=0;i<c.keys;++i){acc+=1/std::pow(double(i+1),c.theta)/sum;cdf[i]=acc;}
        cdf.back()=1;
    }
    Trace trace(c.trace_txns);
    for(std::uint64_t t=0;t<c.trace_txns;++t) {
        std::unordered_set<std::uint64_t> used;
        while(trace[t].size()<c.ops) {
            auto rank=c.theta==0?uniform(rng):std::uint64_t(std::lower_bound(cdf.begin(),cdf.end(),std::generate_canonical<double,53>(rng))-cdf.begin());
            if(!used.insert(rank).second)continue;
            bool ins=insert(rng);
            const auto key=rank*10000+(ins?rng()%10000:0);
            trace[t].push_back({ins,key,std::min(c.keys,rank+c.width)*10000,(t<<32)|trace[t].size()});
        }
    }
    return trace;
}
struct Stats {
    std::uint64_t commits=0,late_commits=0,attempts=0,deadlocks=0,timeouts=0;
    std::uint64_t exhausted=0,unfinished=0,errors=0,scans=0,inserts=0,late_inserts=0;
    std::uint64_t rows=0,checksum=0;
    double elapsed=0,drain=0;
    Stats& operator+=(const Stats& x) {
#define SUM(f) f+=x.f
        SUM(commits);SUM(late_commits);SUM(attempts);SUM(deadlocks);SUM(timeouts);
        SUM(exhausted);SUM(unfinished);SUM(errors);SUM(scans);SUM(inserts);SUM(late_inserts);SUM(rows);SUM(checksum);
#undef SUM
        return *this;
    }
};
Stats run(Index& index,const Trace& trace,const Config& c,double seconds,std::atomic<std::uint64_t>& rid) {
    std::atomic<unsigned> ready{0};std::atomic<bool> go{false};
    Clock::time_point begin,deadline;
    std::vector<Stats> local(c.threads);std::vector<std::thread> workers;
    std::vector<std::exception_ptr> exceptions(c.threads);
    for(unsigned id=0;id<c.threads;++id)workers.emplace_back([&,id]{
        ++ready;while(!go.load(std::memory_order_acquire))std::this_thread::yield();
        try {
            auto& s=local[id];std::size_t pos=id;
            while(Clock::now()<deadline) {
                const auto& ops=trace[pos];
                const auto rid_base=rid.fetch_add(c.ops,std::memory_order_relaxed);
                bool done=false;
                for(unsigned attempt=0;attempt<c.max_attempts;++attempt) {
                    if(Clock::now()>=deadline){++s.unfinished;done=true;break;}
                    auto tx=index.begin();++s.attempts;
                    Status status=Status::Ok;bool expired=false;
                    std::uint64_t scans=0,inserts=0,rows=0,checksum=0;
                    for(std::size_t j=0;j<ops.size();++j) {
                        if(Clock::now()>=deadline){expired=true;break;}
                        const auto& op=ops[j];
                        if(op.insert) {
                            status=index.insert(*tx,{op.key,rid_base+j,op.value});
                            if(status==Status::Ok)++inserts;
                        } else {
                            Index::Cursor cursor;Entry entry;
                            status=index.open(*tx,op.key,Start::GreaterEqual,op.end,Stop::Less,cursor,entry);
                            while(status==Status::Ok) {
                                ++rows;checksum+=entry.value+entry.key+entry.rid;
                                if(Clock::now()>=deadline){expired=true;break;}
                                status=index.next(*tx,cursor,entry);
                            }
                            if(status==Status::NotFound)status=Status::Ok;
                            if(status==Status::Ok && !expired)++scans;
                        }
                        if(status!=Status::Ok || expired)break;
                    }
                    if(status==Status::Ok && !expired && Clock::now()<deadline) {
                        if(index.commit(*tx)!=Status::Ok)throw std::runtime_error("commit failed");
                        if(Clock::now()<deadline){++s.commits;s.scans+=scans;s.inserts+=inserts;s.rows+=rows;s.checksum+=checksum;}
                        else {++s.late_commits;s.late_inserts+=inserts;}
                        done=true;break;
                    }
                    if(index.abort(*tx)!=Status::Ok)throw std::runtime_error("abort failed");
                    if(status==Status::Deadlock)++s.deadlocks;
                    else if(status==Status::Timeout)++s.timeouts;
                    else if(status!=Status::Ok){++s.errors;throw std::runtime_error("unexpected operation status "+std::to_string(int(status)));}
                    if(expired || Clock::now()>=deadline){++s.unfinished;done=true;break;}
                }
                if(!done)++s.exhausted;
                pos+=c.threads;if(pos>=trace.size())pos=id;
            }
        } catch(...) {exceptions[id]=std::current_exception();}
    });
    while(ready.load()!=c.threads)std::this_thread::yield();
    begin=Clock::now();deadline=begin+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    go.store(true,std::memory_order_release);
    for(auto& thread:workers)thread.join();
    const auto end=Clock::now();
    for(auto& error:exceptions)if(error)std::rethrow_exception(error);
    Stats total;for(const auto& s:local)total+=s;
    total.elapsed=seconds;total.drain=std::max(0.,std::chrono::duration<double>(end-deadline).count());
    return total;
}
void stats_json(const Stats& s) {
    std::cout<<"{\"commits\":"<<s.commits<<",\"tps\":"<<s.commits/s.elapsed
      <<",\"late_commits\":"<<s.late_commits<<",\"attempts\":"<<s.attempts
      <<",\"deadlocks\":"<<s.deadlocks<<",\"timeouts\":"<<s.timeouts
      <<",\"exhausted\":"<<s.exhausted<<",\"unfinished\":"<<s.unfinished<<",\"errors\":"<<s.errors
      <<",\"committed_scans\":"<<s.scans<<",\"committed_inserts\":"<<s.inserts<<",\"late_inserts\":"<<s.late_inserts
      <<",\"committed_scan_rows\":"<<s.rows<<",\"checksum\":"<<s.checksum
      <<",\"window_s\":"<<s.elapsed<<",\"drain_s\":"<<s.drain<<"}";
}
int main(int argc,char** argv) try {
    Config c;
    for(int i=1;i<argc;i+=2) {
        if(i+1>=argc)throw std::invalid_argument("missing CLI value");
        std::string k=argv[i],v=argv[i+1];
        if(k=="--keys")c.keys=std::stoull(v);
        else if(k=="--trace-txns")c.trace_txns=std::stoull(v);
        else if(k=="--ops")c.ops=std::stoull(v);
        else if(k=="--range-length")c.width=std::stoull(v);
        else if(k=="--seed")c.seed=std::stoull(v);
        else if(k=="--threads")c.threads=std::stoul(v);
        else if(k=="--order")c.order=std::stoul(v);
        else if(k=="--lock-timeout-ms")c.timeout_ms=std::stoul(v);
        else if(k=="--max-attempts")c.max_attempts=std::stoul(v);
        else if(k=="--theta")c.theta=std::stod(v);
        else if(k=="--warmup")c.warmup=std::stod(v);
        else if(k=="--duration")c.duration=std::stod(v);
        else throw std::invalid_argument("unknown CLI option "+k);
    }
    if(!c.keys || !c.ops || c.ops>c.keys || !c.threads || c.trace_txns<c.threads || c.warmup<=0 || c.duration<=0 || c.theta<0 || !c.max_attempts)
        throw std::invalid_argument("invalid config");
    auto trace=generate(c);
    std::uint64_t trace_inserts=0;for(const auto& tx:trace)for(const auto& op:tx)trace_inserts+=op.insert;
    std::cerr<<"trace ready theta="<<c.theta<<" inserts="<<trace_inserts<<"/"<<c.trace_txns*c.ops<<"\n";
    Index index(c.order,false,std::chrono::milliseconds(c.timeout_ms));
    const auto load_start=Clock::now();
    for(std::uint64_t first=0;first<c.keys;first+=4096) {
        auto tx=index.begin();
        for(auto rank=first;rank<std::min(c.keys,first+4096);++rank)
            if(index.insert(*tx,{rank*10000,rank+1,rank})!=Status::Ok)throw std::runtime_error("initial load failed");
        if(index.commit(*tx)!=Status::Ok)throw std::runtime_error("load commit failed");
        if(first%262144==0)std::cerr<<"loaded "<<std::min(c.keys,first+4096)<<"/"<<c.keys<<"\n";
    }
    const auto load_s=std::chrono::duration<double>(Clock::now()-load_start).count();
    if(!index.validate() || index.size()!=c.keys)throw std::runtime_error("initial validation failed");
    std::cerr<<"load complete "<<load_s<<"s height="<<index.height()<<"; warmup\n";
    std::atomic<std::uint64_t> rid{c.keys+1};
    auto warm=run(index,trace,c,c.warmup,rid);
    const auto warm_size=index.size();
    if(!index.validate() || warm_size!=c.keys+warm.inserts+warm.late_inserts || index.metrics().live_lock_names!=0)
        throw std::runtime_error("warmup validation/accounting failed");
    auto before=index.metrics();
    std::cerr<<"warmup commits="<<warm.commits<<" size="<<warm_size<<"; measure\n";
    auto measure=run(index,trace,c,c.duration,rid);
    const auto final_size=index.size();auto after=index.metrics();
    bool valid=index.validate();
    if(!valid || final_size!=warm_size+measure.inserts+measure.late_inserts || after.live_lock_names!=0)
        throw std::runtime_error("final validation/accounting failed");
    std::cout<<std::setprecision(12)<<"{\"algorithm\":\"aries-kvl-paper\",\"policy\":\"wait-deadlock-detect\",\"theta\":"<<c.theta
      <<",\"keys\":"<<c.keys<<",\"threads\":"<<c.threads<<",\"trace_txns\":"<<c.trace_txns<<",\"ops\":"<<c.ops
      <<",\"range_length\":"<<c.width<<",\"seed\":"<<c.seed<<",\"order\":"<<c.order<<",\"lock_timeout_ms\":"<<c.timeout_ms
      <<",\"max_attempts\":"<<c.max_attempts<<",\"trace_inserts\":"<<trace_inserts<<",\"trace_operations\":"<<c.trace_txns*c.ops
      <<",\"load_s\":"<<load_s<<",\"after_warmup_size\":"<<warm_size<<",\"final_size\":"<<final_size
      <<",\"height\":"<<index.height()<<",\"valid\":true,\"warmup\":";
    stats_json(warm);std::cout<<",\"measurement\":";stats_json(measure);
    std::cout<<",\"metrics\":{\"lock_requests\":"<<after.lock_requests-before.lock_requests
      <<",\"lock_waits\":"<<after.lock_waits-before.lock_waits<<",\"instant_requests\":"<<after.instant_requests-before.instant_requests
      <<",\"revalidations\":"<<after.revalidations-before.revalidations<<",\"replications\":"<<after.replications-before.replications
      <<",\"splits\":"<<after.splits-before.splits<<",\"physical_inserts\":"<<after.physical_inserts-before.physical_inserts
      <<",\"undo_actions\":"<<after.undo_actions-before.undo_actions<<",\"cursor_fast\":"<<after.cursor_fast-before.cursor_fast
      <<",\"cursor_repositions\":"<<after.cursor_repositions-before.cursor_repositions
      <<",\"live_lock_names\":"<<after.live_lock_names<<",\"max_page_latches\":"<<after.max_page_latches<<"}}\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"ERROR: "<<e.what()<<"\n";return 1;}
