// Nakazono et al. arXiv:2407.13294, §III-D..G, Algorithms 1/2.
// Adaptation: private writes are installed at commit, so aborted txns do not
// change hash state. Committed membership deltas persist until synchronized.
#include "index/griffin.h"
#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <thread>
#include <unordered_map>

namespace rrl {
struct BTreeGriffin::Impl {
    static constexpr std::size_t shard_count=64;
    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<idx_key_t,void*> values;
    };
    struct Delta {idx_key_t key;bool remove;};
    std::array<Shard,shard_count> hash;
    BTreeLatch ordered;
    // This fence indexes committed replication work, without txn identities.
    // It replaces retention of committed Lu in the index part of Griffin.
    mutable std::shared_mutex fence;
    std::map<idx_key_t,std::size_t> pending;
    std::deque<std::vector<Delta>> queue;
    const std::size_t capacity;
    std::atomic<bool> failed{false},online{false};
    bool initialized{false},stopping{false},busy{false};
    std::atomic<std::size_t> count{0};
    std::condition_variable_any changed;
    std::thread sync;
    std::function<void()> before_sync,after_hash_batch;
    std::uint64_t key_token{0};
    explicit Impl(std::size_t c):capacity(c){}
    static std::size_t shard(idx_key_t key) {
        // Mix, so sequential keys do not concentrate on one hash shard.
        key^=key>>30;key*=0xbf58476d1ce4e5b9ULL;key^=key>>27;
        return (key^(key>>31))%shard_count;
    }
    void require_healthy() const {
        if(failed.load() || !initialized)throw std::runtime_error("Griffin index failed/uninitialized");
    }
    void synchronize() noexcept {
        try {
            for(;;) {
                std::vector<Delta> batch;
                {
                    std::unique_lock<std::shared_mutex> lock(fence);
                    changed.wait(lock,[&]{return stopping || failed || !queue.empty();});
                    if(failed || (stopping && queue.empty()))return;
                    batch=std::move(queue.front());queue.pop_front();busy=true;
                }
                // Test hooks run outside the fence, like a delayed sync thread.
                if(before_sync)before_sync();
                {
                    std::unique_lock<std::shared_mutex> lock(fence);
                    try {
                    for(const auto& d:batch) {
                        const auto rc=d.remove?ordered.erase(d.key):ordered.insert(d.key,&key_token);
                        if(rc!=RCOK && !(d.remove && rc==Abort))
                            throw std::runtime_error("Griffin ordered replication failed");
                    }
                    // Apply the ENTIRE committed batch before retiring fences.
                    for(const auto& d:batch) {
                        const auto found=pending.find(d.key);
                        if(found==pending.end() || !found->second)throw std::runtime_error("Griffin pending underflow");
                        if(!--found->second)pending.erase(found);
                    }
                    busy=false;changed.notify_all();
                    } catch(...) {failed=true;changed.notify_all();throw;}
                }
            }
        } catch(...) {
            // Publish with the same mutex as wait_synced's predicate, so a
            // failure outside the apply critical section cannot lose a wakeup.
            std::unique_lock<std::shared_mutex> lock(fence);
            failed=true;changed.notify_all(); // Retain pending fences, fail closed.
        }
    }
};
BTreeGriffin::BTreeGriffin(std::size_t capacity):impl_(new Impl(capacity)) {
    if(!capacity || capacity>1048576)throw std::invalid_argument("Griffin queue capacity must be 1..1048576");
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
        // The uniform driver seeds through insert before worker startup.
        // Preload both structures directly, without one queued batch per row.
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
            } catch(...) {p.failed=true;p.changed.notify_all();throw;}
        } catch(...) {
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
    std::vector<Impl::Delta> delta;delta.reserve(changes.size());
    try {
        std::unique_lock<std::shared_mutex> fence(p.fence);
        if(p.failed)return ERROR;
        // Conservative preflight BEFORE the first hash mutation.
        if(p.queue.size()+static_cast<std::size_t>(p.busy)>=p.capacity)return AbortCapacity;
        std::array<bool,Impl::shard_count> used{};
        for(const auto& c:changes)used[Impl::shard(c.key)]=true;
        std::array<std::unique_lock<std::shared_mutex>,Impl::shard_count> guards;
        for(std::size_t n=0;n<used.size();++n)if(used[n])guards[n]=std::unique_lock<std::shared_mutex>(p.hash[n].mutex);
        try {
        for(const auto& c:changes) {
            auto& table=p.hash[Impl::shard(c.key)].values;
            const auto found=table.find(c.key);
            if(c.remove) {
                if(found!=table.end()){table.erase(found);--p.count;delta.push_back({c.key,true});}
            } else if(found==table.end()) {
                table.emplace(c.key,const_cast<std::uint64_t*>(c.value));++p.count;delta.push_back({c.key,false});
            } else found->second=const_cast<std::uint64_t*>(c.value);
        }
        if(p.after_hash_batch)p.after_hash_batch();
        if(!delta.empty()) {
            for(const auto& d:delta)++p.pending[d.key];
            p.queue.push_back(std::move(delta));p.changed.notify_all();
        }
        return RCOK;
        } catch(...) {p.failed=true;p.changed.notify_all();throw;}
    } catch(...) {
        std::unique_lock<std::shared_mutex> fence(p.fence);
        p.failed=true;p.changed.notify_all();return ERROR;
    }
}
bool BTreeGriffin::lookup(idx_key_t key,void*& value) const {
    auto& p=*impl_;p.require_healthy();
    auto& shard=p.hash[Impl::shard(key)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    p.require_healthy(); // A failed batch must be visible before shard unlock.
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
    // Exactly one ordered traversal; values are read from the hash table.
    p.ordered.scan_range_into(range,out);
    for(auto& row:out) {
        void* value=nullptr;
        if(!lookup(row.key,value))throw std::runtime_error("Griffin unfenced stale key");
        row.value=value;
    }
}
std::vector<KeyValue> BTreeGriffin::scan_range(const KeyRangeEnt& range) const {
    std::vector<KeyValue> out;scan_range_into(range,out);return out;
}
std::size_t BTreeGriffin::size() const {return impl_->count.load();}
void BTreeGriffin::wait_synced() const {
    auto& p=*impl_;p.require_healthy();
    std::unique_lock<std::shared_mutex> lock(p.fence);
    p.changed.wait(lock,[&]{return p.failed || (p.queue.empty() && !p.busy);});
    p.require_healthy();
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
// Defined in the implementation so private synchronization state stays out
// of public headers. Tests declare the same friend method signature.
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
