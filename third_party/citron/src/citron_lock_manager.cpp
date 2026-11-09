#include "citron/citron_lock_manager.h"
#include "citron/backends.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <unordered_map>
namespace citron {
namespace {
// Unit coordinates represent keys in [0, UINT64_MAX). UINT64_MAX is outside
// the configured workload domain: fail explicitly instead of wrapping +1.
bool units_for(const rrl::KeyRangeEnt& r,std::uint64_t units,std::uint64_t& lo,std::uint64_t& hi) {
    if(!r.valid())throw std::invalid_argument("invalid Citron interval");
    if(r.empty()){lo=hi=0;return false;}
    lo=r.has_start?r.StartKey:0;
    if(r.has_start && r.start_bound==rrl::Bound::Open) {
        if(lo==UINT64_MAX){hi=lo;return false;}++lo;
    }
    hi=r.has_end?r.EndKey:units;
    if(r.has_end && r.end_bound==rrl::Bound::Closed) {
        if(hi==UINT64_MAX)throw std::out_of_range("Citron key domain exceeds configured tree");
        ++hi;
    }
    if(lo>=units || hi>units)throw std::out_of_range("Citron interval exceeds configured tree");
    return lo<hi;
}
struct Session {
    std::unique_ptr<CitronClient> client;
};
struct Held {
    std::uint64_t lo,hi;
    RangeLock physical;
};
struct Txn {
    mutable std::mutex mutex;
    std::vector<Held> held;
};
struct Directory {
    mutable std::mutex mutex;
    std::unordered_map<rrl::txnid_t,std::shared_ptr<Txn>> txns;
};
}
struct CitronLockManager::Impl {
    LockManagerConfig config;
    SegmentTree tree;
    std::vector<std::uint64_t> words;
    std::array<Directory,64> directories;
    mutable std::mutex sessions_mutex;
    std::map<std::thread::id,std::shared_ptr<Session>> sessions;
    std::atomic<std::uint64_t> calls{0},grants{0},aborts{0},errors{0},unlocks{0},release_errors{0},shared{0};
    explicit Impl(LockManagerConfig c):config(std::move(c)),tree(config.tree_height) {
        if(!config.no_wait || !config.max_sessions || !config.lock_budget)
            throw std::invalid_argument("Citron requires no-wait, bounded sessions and lock budget");
        config.client.abort_on_conflict=true;
        if(config.server_host.empty())words.resize((tree.total_nodes()+2)*kNodeWords);
    }
    std::shared_ptr<Session> session() {
        thread_local std::unordered_map<const Impl*,std::weak_ptr<Session>> cache;
        if(auto existing=cache[this].lock())return existing;
        for(auto it=cache.begin();it!=cache.end();) {
            if(it->second.expired())it=cache.erase(it);else ++it;
        }
        std::lock_guard<std::mutex> guard(sessions_mutex);
        const auto id=std::this_thread::get_id();
        const auto found=sessions.find(id);if(found!=sessions.end()){cache[this]=found->second;return found->second;}
        if(sessions.size()>=config.max_sessions)throw std::runtime_error("Citron session capacity exhausted");
        auto out=std::make_shared<Session>();std::unique_ptr<Backend> backend;
        if(config.server_host.empty())backend=std::make_unique<LocalBackend>(words.data(),tree.total_nodes()+2);
        else {
            auto remote=std::make_unique<RdmaBackend>(config.server_host,config.server_port);
            if(remote->region().length!=region_bytes(tree.total_nodes()+2))
                throw std::runtime_error("Citron server/client geometry mismatch");
            backend=std::move(remote);
        }
        out->client=std::make_unique<CitronClient>(std::move(backend),tree,config.client);
        sessions.emplace(id,out);cache[this]=out;return out;
    }
    std::shared_ptr<Txn> state(rrl::txnid_t id,bool create) {
        auto& dir=directories[id%directories.size()];std::lock_guard<std::mutex> guard(dir.mutex);
        auto it=dir.txns.find(id);if(it!=dir.txns.end())return it->second;
        if(!create)return {};
        auto out=std::make_shared<Txn>();out->held.reserve(config.lock_budget);
        dir.txns.emplace(id,out);return out;
    }
    void forget(rrl::txnid_t id) {
        auto& dir=directories[id%directories.size()];std::lock_guard<std::mutex> guard(dir.mutex);dir.txns.erase(id);
    }
};
CitronLockManager::CitronLockManager(LockManagerConfig c):impl_(new Impl(std::move(c))){}
CitronLockManager::~CitronLockManager()=default;
const SegmentTree& CitronLockManager::tree() const{return impl_->tree;}
void CitronLockManager::prepare_lock_thread(std::size_t){impl_->session();}
rrl::RC CitronLockManager::lock(rrl::txnid_t id,rrl::LockMode mode,const rrl::KeyRangeEnt& range,rrl::LockKind kind,std::vector<rrl::txnid_t>* conflicts) {
    auto& p=*impl_;++p.calls;if(conflicts)conflicts->clear();
    if(!id || (mode!=rrl::LockMode::S && mode!=rrl::LockMode::X) ||
       (kind!=rrl::LockKind::Predicate && kind!=rrl::LockKind::ExactRange)){++p.errors;return rrl::ERROR;}
    if(mode==rrl::LockMode::S && !p.config.shared_as_exclusive){++p.errors;return rrl::ERROR;}
    std::uint64_t lo=0,hi=0;
    try {
        if(!units_for(range,p.tree.units(),lo,hi))return rrl::RCOK;
        auto session=p.session();auto state=p.state(id,true);
        std::lock_guard<std::mutex> guard(state->mutex);
        if(mode==rrl::LockMode::S)++p.shared;
        // Subtract already protected PHYSICAL spans, never unlock-and-relock
        // to extend a transaction. Cached coverage remains held until End.
        std::vector<std::pair<std::uint64_t,std::uint64_t>> protected_spans;
        for(const auto& held:state->held)for(const auto& n:held.physical.nodes) {
            const auto a=n.node.leaf?std::max(n.node.lo,held.physical.lo):n.node.lo;
            const auto b=n.node.leaf?std::min(n.node.hi,held.physical.hi):n.node.hi;
            protected_spans.emplace_back(a,b);
        }
        std::sort(protected_spans.begin(),protected_spans.end());
        std::vector<std::pair<std::uint64_t,std::uint64_t>> missing;
        auto cursor=lo;
        for(const auto& span:protected_spans) {
            if(span.second<=cursor || span.first>=hi)continue;
            if(span.first>cursor)missing.emplace_back(cursor,std::min(span.first,hi));
            cursor=std::max(cursor,span.second);if(cursor>=hi)break;
        }
        if(cursor<hi)missing.emplace_back(cursor,hi);
        for(const auto& part:missing) {
            bool refine=false;
            for(const auto& node:p.tree.cover(part.first,part.second,p.config.client.k)) {
                const auto a=node.leaf?std::max(node.lo,part.first):node.lo;
                const auto b=node.leaf?std::min(node.hi,part.second):node.hi;
                for(const auto& span:protected_spans)if(a<span.second && span.first<b)refine=true;
            }
            // A coarse knapsack cover may include our own earlier node. Exact
            // leaf chunks avoid self-conflict without dropping protection.
            for(auto a=part.first;a<part.second;) {
                const auto b=refine?std::min(part.second,(a/64+1)*64):part.second;
                if(state->held.size()>=p.config.lock_budget){++p.aborts;return rrl::AbortCapacity;}
                state->held.push_back({a,b,{}});auto& held=state->held.back();
                const auto result=session->client->acquire(a,b,true,held.physical);
                if(result!=CitronClient::Status::Acquired) {
                    if(result!=CitronClient::Status::Error)state->held.pop_back();
                    if(result==CitronClient::Status::Error){++p.errors;return rrl::ERROR;}
                    ++p.aborts;return rrl::Abort;
                }
                for(const auto& node:held.physical.nodes)protected_spans.emplace_back(
                    node.node.leaf?std::max(node.node.lo,a):node.node.lo,
                    node.node.leaf?std::min(node.node.hi,b):node.node.hi);
                a=b;
            }
        }
        ++p.grants;return rrl::RCOK;
    }catch(const std::exception&){++p.errors;return rrl::ERROR;}
}
rrl::RC CitronLockManager::unlock_all(rrl::txnid_t id) {
    auto& p=*impl_;++p.unlocks;auto state=p.state(id,false);if(!state)return rrl::RCOK;
    try {
        auto session=p.session();std::lock_guard<std::mutex> guard(state->mutex);
        while(!state->held.empty()) {
            if(!session->client->release(state->held.back().physical)){++p.release_errors;return rrl::ERROR;}
            state->held.pop_back();
        }
        p.forget(id);return rrl::RCOK;
    }catch(const std::exception&){++p.release_errors;return rrl::ERROR;}
}
rrl::RC CitronLockManager::unlock(rrl::txnid_t id,const rrl::KeyRangeEnt& range,rrl::LockKind) {
    // Strict adapter supports only exact physical-acquisition release; a
    // subsumed logical request must not remove a larger held physical lock.
    auto& p=*impl_;++p.unlocks;auto state=p.state(id,false);if(!state)return rrl::RCOK;
    try {
        std::uint64_t lo=0,hi=0;if(!units_for(range,p.tree.units(),lo,hi))return rrl::RCOK;
        auto session=p.session();std::lock_guard<std::mutex> guard(state->mutex);
        for(auto it=state->held.begin();it!=state->held.end();++it)if(it->lo==lo && it->hi==hi) {
            if(!session->client->release(it->physical)){++p.release_errors;return rrl::ERROR;}
            state->held.erase(it);break;
        }
        if(state->held.empty())p.forget(id);return rrl::RCOK;
    }catch(const std::exception&){++p.release_errors;return rrl::ERROR;}
}
std::size_t CitronLockManager::get_lock_count(rrl::txnid_t id) const {
    auto& p=*impl_;std::vector<std::shared_ptr<Txn>> states;
    for(auto& dir:p.directories) {
        std::lock_guard<std::mutex> guard(dir.mutex);
        for(const auto& entry:dir.txns)if(!id || entry.first==id)states.push_back(entry.second);
    }
    std::size_t count=0;for(const auto& state:states){std::lock_guard<std::mutex> guard(state->mutex);count+=state->held.size();}return count;
}
CitronLockManager::Diagnostics CitronLockManager::diagnostics() const {
    auto& p=*impl_;Diagnostics d;d.lock_calls=p.calls;d.lock_grants=p.grants;d.lock_aborts=p.aborts;d.lock_errors=p.errors;
    d.unlock_calls=p.unlocks;d.unlock_failures=p.release_errors;d.shared_mapped_to_exclusive=p.shared;
    std::lock_guard<std::mutex> guard(p.sessions_mutex);d.sessions=p.sessions.size();
    if(!p.config.server_host.empty())for(const auto& entry:p.sessions)d.rdma_operations+=entry.second->client->operations();return d;
}
void CitronLockManager::reset_diagnostics() {
    auto& p=*impl_;p.calls=0;p.grants=0;p.aborts=0;p.errors=0;p.unlocks=0;p.release_errors=0;p.shared=0;
}
} // namespace citron
