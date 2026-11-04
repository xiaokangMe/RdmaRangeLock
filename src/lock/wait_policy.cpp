#include "lock/wait_policy.h"
#include "common/execution_context.h"
#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace rrl {
using Clock = std::chrono::steady_clock;
WaitPolicy parse_wait_policy(const std::string& v) {
    if (v == "no-wait") return WaitPolicy::NoWait;
    if (v == "wait-die") return WaitPolicy::WaitDie;
    if (v == "wound-wait") return WaitPolicy::WoundWait;
    throw std::invalid_argument("wait-policy must be no-wait, wait-die or wound-wait");
}
const char* wait_policy_name(WaitPolicy p) {
    return p == WaitPolicy::NoWait ? "no-wait" : p == WaitPolicy::WaitDie ? "wait-die" : "wound-wait";
}
struct TransactionCoordinator::Impl {
    struct Txn {
        std::uint64_t session, age;
        WaitPolicy policy;
        Clock::time_point deadline;
        bool wounded{false}, committing{false}, waiting{false};
        // A reusable fixed reservation avoids growth while the txn holds locks.
        std::vector<RangeLock> locks;
    };
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::map<txnid_t, Txn> txns;
    std::map<std::uint64_t,std::uint64_t> session_ages;
    std::uint64_t next{1};
    bool poisoned{false};
    std::size_t lock_budget{16384};
    bool exact_objects{false};
    WaitPolicy cohort_policy{WaitPolicy::NoWait};
    Txn* owned(std::uint64_t session, txnid_t id) {
        const auto it = txns.find(id);
        return it == txns.end() || it->second.session != session ? nullptr : &it->second;
    }
    TxnResult status(const Txn* t) const {
        if (poisoned || !t) return TxnResult::Error;
        if (t->wounded) return TxnResult::Wounded;
        if (Clock::now() >= t->deadline) return TxnResult::Deadline;
        return TxnResult::Ok;
    }
};
TransactionCoordinator::TransactionCoordinator(std::size_t lock_budget,bool exact_objects): impl_(new Impl) {
    if(!lock_budget || lock_budget>1048576)throw std::invalid_argument("lock budget must be 1..1048576");
    impl_->lock_budget=lock_budget;impl_->exact_objects=exact_objects;
}
TransactionCoordinator::~TransactionCoordinator() = default;
PolicyReply TransactionCoordinator::begin(std::uint64_t session, WaitPolicy policy,
                                         std::uint64_t age, std::uint64_t budget_ns) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->poisoned || !budget_ns || budget_ns > 600000000000ULL ||
        policy > WaitPolicy::WoundWait || impl_->next >= (1ULL << 47)) return {};
    // An age may only be inherited from this session's previous begin. Ages
    // are issued globally by the server, never compared across host clocks.
    for(const auto& [id,t]:impl_->txns){(void)id;if(t.session==session)return {};}
    // Mixing age protocols in one active wait-for graph invalidates either
    // protocol's acyclicity argument. Sequential cases may switch freely.
    if(!impl_->txns.empty() && impl_->cohort_policy!=policy)return {};
    impl_->cohort_policy=policy;
    if (age && impl_->session_ages[session]!=age) return {};
    const auto id = impl_->next++;
    Impl::Txn txn{session, age ? age : id, policy, Clock::now()+std::chrono::nanoseconds(budget_ns),false,false,false,{}};
    txn.locks.reserve(impl_->lock_budget);
    impl_->session_ages[session]=txn.age;
    impl_->txns.emplace(id,std::move(txn));
    return {TxnResult::Ok,id,0,age ? age : id};
}
PolicyReply TransactionCoordinator::acquire(std::uint64_t session, txnid_t id, LockMode mode,
                                           const KeyRangeEnt& range, LockKind kind) {
    if (!range.valid() || range.empty() || mode > LockMode::X || kind > LockKind::Node) return {};
    std::unique_lock<std::mutex> guard(impl_->mutex);
    auto* txn = impl_->owned(session,id);
    const auto initial = impl_->status(txn);
    if (initial != TxnResult::Ok) return {initial,id,0};
    if (txn->committing || txn->waiting) return {};
    RangeLock request{id,mode,kind,range};
    const auto same = [&](const RangeLock& a) {
        const auto& r=a.range;
        return a.kind==kind && r.StartKey==range.StartKey && r.EndKey==range.EndKey &&
            r.has_start==range.has_start && r.has_end==range.has_end &&
            r.start_bound==range.start_bound && r.end_bound==range.end_bound;
    };
    for (const auto& a:txn->locks) if (same(a)) request.mode=lock_mode_join(request.mode,a.mode);
    const auto start=Clock::now();
    bool waited=false;
    for (;;) {
        const auto status=impl_->status(txn);
        if(status!=TxnResult::Ok) {
            txn->waiting=false;
            return {status,id,waited ? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count()) : 0};
        }
        bool conflict=false, must_die=false;
        for(auto& [other_id,other]:impl_->txns) {
            if(other_id==id)continue;
            bool overlaps=false;
            for(const auto& held:other.locks)if((!impl_->exact_objects || request.kind==held.kind) && request.conflicts(held)){overlaps=true;break;}
            if(!overlaps)continue;
            conflict=true;
            if(txn->policy==WaitPolicy::WaitDie && other.age<txn->age)must_die=true;
            if(txn->policy==WaitPolicy::WoundWait && txn->age<other.age && !other.committing) {
                other.wounded=true;
                impl_->changed.notify_all(); // also wake a wounded victim waiting on another key
            }
        }
        if(!conflict) {
            auto found=std::find_if(txn->locks.begin(),txn->locks.end(),same);
            if(found!=txn->locks.end())found->mode=request.mode;
            else {
                if(txn->locks.size()==txn->locks.capacity()){txn->waiting=false;return {TxnResult::Error,id,0};}
                txn->locks.push_back(request);
            }
            txn->waiting=false;
            return {TxnResult::Ok,id,waited ? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count()) : 0};
        }
        if(txn->policy==WaitPolicy::NoWait || must_die) {
            txn->waiting=false;
            return {TxnResult::Conflict,id,waited ? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count()) : 0};
        }
        waited=true;txn->waiting=true;
        if(coroutine_active()) {
            guard.unlock();cooperative_yield();guard.lock();
        } else impl_->changed.wait_until(guard,txn->deadline);
    }
}
PolicyReply TransactionCoordinator::check(std::uint64_t session,txnid_t id) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return {impl_->status(impl_->owned(session,id)),id,0};
}
PolicyReply TransactionCoordinator::prepare_commit(std::uint64_t session,txnid_t id) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    auto* t=impl_->owned(session,id);
    const auto status=impl_->status(t);
    if(status!=TxnResult::Ok)return {status,id,0};
    if(t->waiting)return {};
    t->committing=true;
    return {TxnResult::Ok,id,0};
}
PolicyReply TransactionCoordinator::end(std::uint64_t session,txnid_t id) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    auto* t=impl_->owned(session,id);
    if(!t || t->waiting)return {};
    impl_->txns.erase(id);
    impl_->changed.notify_all();
    return {TxnResult::Ok,id,0};
}
void TransactionCoordinator::disconnect(std::uint64_t session) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    // Keep metadata protecting uncertain physical locks. Fail closed globally
    // instead of granting a waiter while a disconnected NIC may still write.
    for(const auto& [id,t]:impl_->txns) {
        (void)id;
        if(t.session==session){impl_->poisoned=true;break;}
    }
    impl_->changed.notify_all();
    impl_->session_ages.erase(session);
}
void TransactionCoordinator::shutdown() {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->poisoned=true;impl_->changed.notify_all();
}
std::size_t TransactionCoordinator::active() const {
    std::lock_guard<std::mutex> guard(impl_->mutex);return impl_->txns.size();
}
std::size_t TransactionCoordinator::waiting() const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    std::size_t n=0;for(const auto& [id,t]:impl_->txns){(void)id;n+=t.waiting;}return n;
}
} // namespace rrl
