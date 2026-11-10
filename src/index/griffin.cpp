// Nakazono et al. arXiv:2407.13294, §III-D..G, Algorithms 1/2.
// Private writes are installed at commit. Committed membership deltas persist
// until synchronized; batching/coalescing never shortens that protection.
#include "index/griffin.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <thread>
#include <unordered_map>

namespace rrl {
struct BTreeGriffin::Impl {
    using Clock=std::chrono::steady_clock;
    static std::uint64_t elapsed(Clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
    }
    static constexpr std::size_t shard_count=64;
    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<idx_key_t,void*> values;
    };
    struct Delta {idx_key_t key;bool remove;};
    struct Batch {std::vector<Delta> changes;Clock::time_point published;};
    std::array<Shard,shard_count> hash;
    BTreeLatch ordered;
    // Committed replication metadata, without txn identities or lock grants.
    mutable std::shared_mutex fence;
    std::map<idx_key_t,std::size_t> pending;
    std::deque<Batch> queue;
    const std::size_t capacity,sync_limit;
    std::size_t busy_batches{0};
    std::atomic<bool> failed{false},online{false};
    bool initialized{false},stopping{false};
    std::atomic<std::size_t> count{0};
    GriffinDiagnostics metrics;
    LatencyHistogram sync_work,sync_lag;
    std::condition_variable_any changed;
    std::thread sync;
    std::function<void()> before_sync,after_hash_batch;
    std::uint64_t key_token{0};
    explicit Impl(std::size_t c,std::size_t l):capacity(c),sync_limit(l){}
    static std::size_t shard(idx_key_t key) {
        key^=key>>30;key*=0xbf58476d1ce4e5b9ULL;key^=key>>27;
        return (key^(key>>31))%shard_count;
    }
    void require_healthy() const {
        if(failed.load() || !initialized)throw std::runtime_error("Griffin index failed/uninitialized");
    }
    void synchronize() noexcept {
        try {
            std::vector<Batch> group;group.reserve(sync_limit);
            for(;;) {
                group.clear();
                {
                    std::unique_lock<std::shared_mutex> lock(fence);
                    changed.wait(lock,[&]{return stopping || failed || !queue.empty();});
                    if(failed || (stopping && queue.empty()))return;
                    const auto n=std::min(sync_limit,queue.size());
                    for(std::size_t i=0;i<n;++i) {
                        group.push_back(std::move(queue.front()));queue.pop_front();
                    }
                    busy_batches=group.size();
                }
                if(before_sync)before_sync();
                const auto started=Clock::now();
                // FIFO last state wins for each key. Pending counts for ALL
                // selected batches remain until the ENTIRE group is applied.
                std::map<idx_key_t,bool> final;
                for(const auto& batch:group)for(const auto& d:batch.changes)final[d.key]=d.remove;
                // No fence across tree work: disjoint scans/commits can run.
                // Intersecting scans still find pending and abort, while the
                // common BTreeLatch protects splits and leaf traversal.
                for(const auto& d:final) {
                    if(failed)return;
                    const auto rc=d.second?ordered.erase(d.first):ordered.insert(d.first,&key_token);
                    if(rc!=RCOK && !(d.second && rc==Abort))
                        throw std::runtime_error("Griffin ordered replication failed");
                }
                const auto work_ns=elapsed(started);
                {
                    std::unique_lock<std::shared_mutex> lock(fence);
                    try {
                        if(failed)return;
                        for(const auto& batch:group)for(const auto& d:batch.changes) {
                            const auto found=pending.find(d.key);
                            if(found==pending.end() || !found->second)
                                throw std::runtime_error("Griffin pending underflow");
                            if(!--found->second)pending.erase(found);
                        }
                        ++metrics.sync_cycles;metrics.synced_batches+=group.size();
                        metrics.tree_operations+=final.size();sync_work.add(work_ns);
                        for(const auto& batch:group)sync_lag.add(elapsed(batch.published));
                        busy_batches=0;changed.notify_all();
                    }catch(...){failed=true;changed.notify_all();throw;}
                }
            }
        }catch(...) {
            failed=true;
            // Same mutex as the wait predicate; no missed failure wakeup.
            std::unique_lock<std::shared_mutex> lock(fence);
            changed.notify_all();
        }
    }
};
BTreeGriffin::BTreeGriffin(std::size_t capacity,std::size_t limit):impl_(new Impl(capacity,limit)) {
    if(!capacity || capacity>1048576)throw std::invalid_argument("Griffin queue capacity must be 1..1048576");
    if(!limit || limit>1048576)throw std::invalid_argument("Griffin sync batch limit must be 1..1048576");
}
BTreeGriffin::~BTreeGriffin() {
    if(impl_->sync.joinable()) {
        {std::unique_lock<std::shared_mutex> lock(impl_->fence);impl_->stopping=true;}
        impl_->changed.notify_all();impl_->sync.join();
    }
}
RC BTreeGriffin::init(std::size_t order) {
    if(impl_->initialized || impl_->ordered.init(order)!=RCOK)return ERROR;
    impl_->initialized=true;
    impl_->sync=std::thread([this]{impl_->synchronize();});
    return RCOK;
}
void BTreeGriffin::prepare_thread(std::size_t,std::size_t) {
    impl_->require_healthy();impl_->online=true;
}
bool BTreeGriffin::healthy() const {return impl_->initialized && !impl_->failed.load();}
RC BTreeGriffin::insert(idx_key_t key,void* value) {
    if(!impl_->online) {
        auto& p=*impl_;if(!healthy())return ERROR;
        try {
            std::unique_lock<std::shared_mutex> fence(p.fence);
            auto& shard=p.hash[Impl::shard(key)];
            std::unique_lock<std::shared_mutex> lock(shard.mutex);
            try {
                const auto rc=p.ordered.insert(key,&p.key_token);if(rc!=RCOK)return rc;
                const auto inserted=shard.values.emplace(key,value);
                if(inserted.second)++p.count;else inserted.first->second=value;
                return RCOK;
            }catch(...){p.failed=true;p.changed.notify_all();throw;}
        }catch(...) {
            std::unique_lock<std::shared_mutex> fence(p.fence);
            p.failed=true;p.changed.notify_all();return ERROR;
        }
    }
    return apply_batch({{key,static_cast<const std::uint64_t*>(value),false}});
}
RC BTreeGriffin::erase(idx_key_t key) {return apply_batch({{key,nullptr,true}});}
RC BTreeGriffin::apply_batch(const std::vector<IndexMutation>& changes) {
    auto& p=*impl_;if(!healthy())return ERROR;
    if(changes.empty())return RCOK;
    for(const auto& c:changes)if(!c.remove && !c.value)return ERROR;
    // A private batch is only visible at commit: intermediate delete/insert
    // states need not be replicated, only the final membership and value.
    std::map<idx_key_t,const IndexMutation*> final;
    std::vector<Impl::Delta> delta;delta.reserve(changes.size());
    for(const auto& c:changes)final[c.key]=&c;
    try {
        std::unique_lock<std::shared_mutex> fence(p.fence);
        if(p.failed)return ERROR;
        std::array<bool,Impl::shard_count> used{};
        for(const auto& c:final)used[Impl::shard(c.first)]=true;
        std::array<std::unique_lock<std::shared_mutex>,Impl::shard_count> guards;
        for(std::size_t n=0;n<used.size();++n)if(used[n])guards[n]=std::unique_lock<std::shared_mutex>(p.hash[n].mutex);
        try {
            for(const auto& c:final) {
                const auto& table=p.hash[Impl::shard(c.first)].values;
                const bool exists=table.find(c.first)!=table.end(),will_exist=!c.second->remove;
                if(exists!=will_exist)delta.push_back({c.first,!will_exist});
            }
            // Capacity is needed only for actual membership replication.
            // Decide before any mutation; a mixed rejected batch is all-or-none.
            if(!delta.empty() && p.queue.size()+p.busy_batches>=p.capacity) {
                ++p.metrics.capacity_aborts;return AbortCapacity;
            }
            for(const auto& c:final) {
                auto& table=p.hash[Impl::shard(c.first)].values;
                const auto found=table.find(c.first);
                if(c.second->remove) {
                    if(found!=table.end()){table.erase(found);--p.count;}
                }else if(found==table.end()) {
                    table.emplace(c.first,const_cast<std::uint64_t*>(c.second->value));++p.count;
                }else found->second=const_cast<std::uint64_t*>(c.second->value);
            }
            if(p.after_hash_batch)p.after_hash_batch();
            if(!delta.empty()) {
                for(const auto& d:delta)++p.pending[d.key];
                p.metrics.membership_operations+=delta.size();++p.metrics.membership_batches;
                p.queue.push_back({std::move(delta),Impl::Clock::now()});
                p.metrics.queue_peak_batches=std::max<std::uint64_t>(p.metrics.queue_peak_batches,p.queue.size()+p.busy_batches);
                p.metrics.pending_peak_keys=std::max<std::uint64_t>(p.metrics.pending_peak_keys,p.pending.size());
                p.changed.notify_all();
            }else ++p.metrics.value_only_batches;
            ++p.metrics.committed_batches;
            return RCOK;
        }catch(...){p.failed=true;p.changed.notify_all();throw;}
    }catch(...) {
        std::unique_lock<std::shared_mutex> fence(p.fence);
        p.failed=true;p.changed.notify_all();return ERROR;
    }
}
bool BTreeGriffin::lookup(idx_key_t key,void*& value) const {
    auto& p=*impl_;p.require_healthy();
    auto& shard=p.hash[Impl::shard(key)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    p.require_healthy();
    const auto found=shard.values.find(key);
    value=found==shard.values.end()?nullptr:found->second;
    return found!=shard.values.end();
}
void BTreeGriffin::scan_range_into(const KeyRangeEnt& range,std::vector<KeyValue>& out) const {
    out.clear();if(range.empty())return;
    auto& p=*impl_;p.require_healthy();
    std::shared_lock<std::shared_mutex> fence(p.fence);
    if(p.failed)throw std::runtime_error("Griffin sync failed");
    auto found=range.has_start?p.pending.lower_bound(range.StartKey):p.pending.begin();
    for(;found!=p.pending.end();++found) {
        if(range.has_end && (found->first>range.EndKey ||
           (found->first==range.EndKey && range.end_bound==Bound::Open)))break;
        if(range.contains(found->first))throw IndexSyncPending();
    }
    p.ordered.scan_range_into(range,out);
    for(auto& row:out) {
        void* value=nullptr;
        if(!lookup(row.key,value))throw std::runtime_error("Griffin unfenced stale key");
        row.value=value;
    }
    p.require_healthy();
}
std::vector<KeyValue> BTreeGriffin::scan_range(const KeyRangeEnt& range) const {
    std::vector<KeyValue> out;scan_range_into(range,out);return out;
}
std::size_t BTreeGriffin::size() const {return impl_->count.load();}
void BTreeGriffin::wait_synced() const {
    auto& p=*impl_;p.require_healthy();
    std::unique_lock<std::shared_mutex> lock(p.fence);
    p.changed.wait(lock,[&]{return p.failed || (p.queue.empty() && !p.busy_batches);});
    p.require_healthy();
}
GriffinDiagnostics BTreeGriffin::diagnostics() const {
    auto& p=*impl_;std::shared_lock<std::shared_mutex> lock(p.fence);
    auto d=p.metrics;d.queue_batches=p.queue.size()+p.busy_batches;d.pending_keys=p.pending.size();
    d.sync_work=p.sync_work.summary();d.sync_lag=p.sync_lag.summary();return d;
}
void BTreeGriffin::reset_diagnostics() {
    wait_synced();auto& p=*impl_;std::unique_lock<std::shared_mutex> lock(p.fence);
    p.require_healthy();
    if(!p.queue.empty() || p.busy_batches)throw std::runtime_error("Griffin reset requires quiescent workers");
    p.metrics={};p.sync_work=LatencyHistogram{};p.sync_lag=LatencyHistogram{};
}
bool BTreeGriffin::validate() const {
    try {
        wait_synced();auto& p=*impl_;
        std::shared_lock<std::shared_mutex> fence(p.fence);
        if(!p.ordered.validate() || !p.pending.empty())return false;
        const auto rows=p.ordered.scan_range(KeyRangeEnt::unbounded());
        if(rows.size()!=size())return false;
        std::size_t hashed=0;
        for(auto& shard:p.hash) {
            std::shared_lock<std::shared_mutex> lock(shard.mutex);hashed+=shard.values.size();
        }
        if(hashed!=rows.size())return false;
        for(const auto& row:rows){void* v=nullptr;if(!lookup(row.key,v))return false;}
        return true;
    }catch(...){return false;}
}
struct BTreeGriffinTestPeer {
    static void before_sync(BTreeGriffin& tree,std::function<void()> hook);
    static void after_hash_batch(BTreeGriffin& tree,std::function<void()> hook);
};
void BTreeGriffinTestPeer::before_sync(BTreeGriffin& tree,std::function<void()> hook) {
    tree.impl_->before_sync=std::move(hook);
}
void BTreeGriffinTestPeer::after_hash_batch(BTreeGriffin& tree,std::function<void()> hook) {
    tree.impl_->after_hash_batch=std::move(hook);
}
} // namespace rrl
