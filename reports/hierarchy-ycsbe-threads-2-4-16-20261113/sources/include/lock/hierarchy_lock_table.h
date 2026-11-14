#pragma once
#include "lock/wait_policy.h"
#include "common/execution_context.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace rrl {
// Fixed-resource local five-mode authority. One generation per context;
// count/owner updates are linearized by the node word, never by a mutex.
class HierarchyLockTable {
public:
    using Word=std::atomic<std::uint64_t>;
    using Clock=std::chrono::steady_clock;
    static constexpr std::size_t max_contexts=64;
    static constexpr std::uint64_t sequence_step=1ULL<<29;
    static constexpr std::uint64_t payload_mask=sequence_step-1;
    static constexpr std::uint64_t owner_mask=127ULL<<21;
    static constexpr std::uint64_t x_flag=1ULL<<28;
    static constexpr std::uint64_t sequence_limit=UINT64_MAX-127*sequence_step;
    enum State:std::uint64_t {Idle,Running,Wounded,Committing};
    struct alignas(64) Slot {Word control{0},age{0};};
    struct alignas(64) LeaseLine {std::atomic<std::uint8_t> bytes[64]{};};
    static_assert(sizeof(Word)==8 && Word::is_always_lock_free,"8-byte lock-free CAS required");
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free,"lock-free leases required");

    HierarchyLockTable(std::size_t nodes,std::size_t contexts,WaitPolicy policy,bool padded=false)
        : nodes_(nodes),contexts_(contexts),policy_(policy),stride_(padded?8:1),lines_((nodes+63)/64) {
        if(!nodes || nodes>UINT32_MAX || !contexts || contexts>64 || nodes>SIZE_MAX/(stride_*8))
            throw std::invalid_argument("invalid compact hierarchy geometry");
        words_.reset(new Word[nodes*stride_]{});slots_.reset(new Slot[contexts]);
        if(policy!=WaitPolicy::NoWait)leases_.reset(new LeaseLine[contexts*lines_]);
    }
    Word* locate(idx_key_t node)const noexcept{return node<nodes_?words_.get()+node*stride_:nullptr;}
    std::size_t lock_bytes()const{return nodes_*stride_*8;}
    std::size_t directory_bytes()const{return 0;}
    std::size_t state_bytes()const{return contexts_*sizeof(Slot);}
    std::size_t lease_bytes()const{return leases_?contexts_*lines_*sizeof(LeaseLine):0;}
    std::size_t node_count()const{return nodes_;}
    void poison(){poisoned_.store(true,std::memory_order_release);}
    bool healthy()const{return !poisoned_.load(std::memory_order_acquire);}
    bool empty()const {
        for(std::size_t n=0;n<nodes_;++n)if(locate(n)->load()&payload_mask)return false;
        if(leases_)for(std::size_t c=0;c<contexts_;++c)for(std::size_t n=0;n<nodes_;++n)
            if(lease(c,n).load())return false;
        return true;
    }
    static bool five_mode(LockMode mode){return mode<=LockMode::SIX;}
    static bool compatible(std::uint64_t w,LockMode mode) {
        const bool owner=w&owner_mask;
        switch(mode) {
            case LockMode::IS:return !(owner && (w&x_flag));
            case LockMode::IX:return !owner && !(w&(127ULL<<14));
            case LockMode::S:return !owner && !(w&(127ULL<<7));
            case LockMode::SIX:return !owner && !(w&((127ULL<<14)|(127ULL<<7)));
            case LockMode::X:return !(w&payload_mask);
            default:return false;
        }
    }
    static std::uint64_t contribution(LockMode mode,std::size_t slot) {
        switch(mode) {
            case LockMode::IS:return 1;
            case LockMode::IX:return 1ULL<<7;
            case LockMode::S:return 1ULL<<14;
            case LockMode::SIX:return (slot+1)<<21;
            case LockMode::X:return ((slot+1)<<21)|x_flag;
            default:return 0;
        }
    }
    class Client {
        struct Held {std::uint32_t node;LockMode mode;};
    public:
        Client(HierarchyLockTable& table,std::size_t slot,std::size_t budget)
            : table_(table),slot_(slot),budget_(budget) {
            if(slot>=table.contexts_ || !budget)throw std::invalid_argument("invalid hierarchy client");
            held_.reserve(budget);
        }
        ~Client(){if(id_ && table_.healthy())finish();}
        Client(const Client&)=delete;Client& operator=(const Client&)=delete;
        PolicyReply begin(std::uint64_t age,std::uint64_t budget_ns) {
            if(id_ || !held_.empty() || !table_.healthy() || !budget_ns || budget_ns>600000000000ULL ||
               (age && age!=age_))return {};
            if(table_.policy_==WaitPolicy::NoWait)age=1;
            else if(!age)age=table_.next_age_.fetch_add(1,std::memory_order_relaxed)+1;
            if(!age || generation_>=(((1ULL<<61)-1)-slot_)/table_.contexts_)return {};
            id_=(++generation_)*table_.contexts_+slot_;age_=age;
            deadline_=Clock::now()+std::chrono::nanoseconds(budget_ns);capacity_abort_=false;
            table_.slots_[slot_].age.store(age,std::memory_order_release);
            table_.slots_[slot_].control.store(control(id_,Running),std::memory_order_release);
            return {TxnResult::Ok,id_,0,age_};
        }
        PolicyReply begin_unordered(std::uint64_t budget_ns){return begin(0,budget_ns);}
        txnid_t id()const{return id_;}
        Clock::time_point deadline()const{return deadline_;}
        std::size_t held_bytes()const{return held_.capacity()*sizeof(Held);}
        bool capacity_abort()const{return capacity_abort_;}
        std::uint64_t cas_retries()const{return cas_retries_;}
        void reset_metrics(){cas_retries_=0;}
        TxnResult check()const {
            if(!id_ || !table_.healthy())return TxnResult::Error;
            const auto c=table_.slots_[slot_].control.load(std::memory_order_acquire);
            if(c==control(id_,Wounded))return TxnResult::Wounded;
            if(c!=control(id_,Running) && c!=control(id_,Committing))return TxnResult::Error;
            return Clock::now()>=deadline_?TxnResult::Deadline:TxnResult::Ok;
        }
        // General API deduplicates and performs atomic mode joins/upgrades.
        template<WaitPolicy Policy>PolicyReply acquire(std::size_t node,LockMode mode) {
            auto found=std::find_if(held_.begin(),held_.end(),[&](const Held& h){return h.node==node;});
            return take<Policy>(node,mode,found==held_.end()?nullptr:&*found);
        }
        // Executor plans are unique and immutable; avoid searching held_.
        template<WaitPolicy Policy>PolicyReply acquire_planned(std::size_t node,LockMode mode) {
            return take<Policy>(node,mode,nullptr);
        }
        template<WaitPolicy Policy>PolicyReply acquire(Word& word,LockMode mode) {
            const auto offset=&word-table_.words_.get();
            if(offset<0 || static_cast<std::size_t>(offset)%table_.stride_)return {};
            return acquire<Policy>(static_cast<std::size_t>(offset)/table_.stride_,mode);
        }
        template<WaitPolicy Policy>TxnResult prepare_commit() {
            const auto status=check();if(status!=TxnResult::Ok)return status;
            if constexpr(Policy==WaitPolicy::WoundWait) {
                auto old=control(id_,Running);
                if(!table_.slots_[slot_].control.compare_exchange_strong(old,control(id_,Committing),std::memory_order_acq_rel))
                    return old==control(id_,Wounded)?TxnResult::Wounded:TxnResult::Error;
            } else table_.slots_[slot_].control.store(control(id_,Committing),std::memory_order_release);
            return TxnResult::Ok;
        }
        void finish()noexcept {
            if(!id_)return;
            for(auto it=held_.rbegin();it!=held_.rend();++it) {
                const auto delta=contribution(it->mode,slot_);
                if(table_.leases_)table_.lease(slot_,it->node).store(transition,std::memory_order_release);
                // Subtraction cannot borrow: this context still owns its
                // contribution; sequence headroom was reserved on acquisition.
                table_.locate(it->node)->fetch_add(sequence_step-delta,std::memory_order_release);
                if(table_.leases_)table_.lease(slot_,it->node).store(0,std::memory_order_release);
            }
            held_.clear();table_.slots_[slot_].control.store(control(id_,Idle),std::memory_order_release);id_=0;
        }
        void abort()noexcept{finish();}
    private:
        static constexpr std::uint8_t transition=128;
        static std::uint64_t control(txnid_t id,State state){return (id<<3)|state;}
        template<WaitPolicy Policy>PolicyReply take(std::size_t node,LockMode mode,Held* held) {
            if(!id_ || node>=table_.nodes_ || !five_mode(mode) || table_.policy_!=Policy)return {};
            const auto target=held?lock_mode_join(held->mode,mode):mode;
            if(held && target==held->mode)return {TxnResult::Ok,id_,0};
            if(!held && held_.size()==budget_){capacity_abort_=true;return {TxnResult::Conflict,id_,0};}
            const auto own=held?contribution(held->mode,slot_):0;
            const auto stable=held?static_cast<std::uint8_t>(held->mode)+1:0;
            auto& atom=*table_.locate(node);auto word=atom.load(std::memory_order_acquire);
            bool waited=false;Clock::time_point wait_start;unsigned collisions=0;
            auto reply=[&](TxnResult result){return PolicyReply{result,id_,waited?
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-wait_start).count()):0};};
            for(;;) {
                if constexpr(Policy==WaitPolicy::WoundWait)
                    if(table_.slots_[slot_].control.load(std::memory_order_acquire)==control(id_,Wounded))return reply(TxnResult::Wounded);
                if(word>=sequence_limit)return reply(TxnResult::Error);
                const auto others=word-own;
                if(compatible(others,target)) {
                    if constexpr(Policy!=WaitPolicy::NoWait)table_.lease(slot_,node).store(transition,std::memory_order_release);
                    const auto desired=others+contribution(target,slot_)+sequence_step;
                    if(atom.compare_exchange_weak(word,desired,std::memory_order_acq_rel,std::memory_order_acquire)) {
                        if(held)held->mode=target;else held_.push_back({static_cast<std::uint32_t>(node),target});
                        if constexpr(Policy!=WaitPolicy::NoWait)table_.lease(slot_,node).store(static_cast<std::uint8_t>(target)+1,std::memory_order_release);
                        if constexpr(Policy==WaitPolicy::WoundWait)
                            if(table_.slots_[slot_].control.load(std::memory_order_acquire)==control(id_,Wounded))return reply(TxnResult::Wounded);
                        return reply(TxnResult::Ok);
                    }
                    if constexpr(Policy!=WaitPolicy::NoWait)table_.lease(slot_,node).store(stable,std::memory_order_release);
                    ++cas_retries_;
                    if(++collisions==32){const auto status=check();if(status!=TxnResult::Ok)return reply(status);collisions=0;}
                    continue;
                }
                if constexpr(Policy==WaitPolicy::NoWait)return reply(TxnResult::Conflict);
                else {
                    const auto status=check();if(status!=TxnResult::Ok)return reply(status);
                    const auto result=blockers<Policy>(node,word,target);
                    if(result!=TxnResult::Ok)return reply(result);
                    if(!waited){waited=true;wait_start=Clock::now();}
                    cooperative_yield();word=atom.load(std::memory_order_acquire);
                }
            }
        }
        template<WaitPolicy Policy>TxnResult blockers(std::size_t node,std::uint64_t snapshot,LockMode target) {
            std::array<std::uint64_t,64> controls{},ages{};std::uint64_t reconstructed=0,mask=0;
            for(std::size_t c=0;c<table_.contexts_;++c) {
                const auto code=table_.lease(c,node).load(std::memory_order_acquire);
                if(code&transition)return TxnResult::Ok; // resample, never treat reservations as owners
                if(!code)continue;
                if(code>5)return TxnResult::Error;
                const auto mode=static_cast<LockMode>(code-1);
                const auto first=table_.slots_[c].control.load(std::memory_order_acquire);
                const auto age=table_.slots_[c].age.load(std::memory_order_acquire);
                const auto second=table_.slots_[c].control.load(std::memory_order_acquire);
                if(first!=second || (first&7)==Idle)return TxnResult::Ok;
                reconstructed+=contribution(mode,c);
                if(c!=slot_ && lock_modes_conflict(mode,target)){mask|=1ULL<<c;controls[c]=first;ages[c]=age;}
            }
            if(table_.locate(node)->load(std::memory_order_acquire)!=snapshot || reconstructed!=(snapshot&payload_mask))return TxnResult::Ok;
            while(mask) {
                const auto c=static_cast<unsigned>(__builtin_ctzll(mask));mask&=mask-1;
                if constexpr(Policy==WaitPolicy::WaitDie){if(age_>ages[c])return TxnResult::Conflict;}
                else if(age_<ages[c] && (controls[c]&7)==Running) {
                    auto expected=controls[c];
                    table_.slots_[c].control.compare_exchange_strong(expected,(expected&~7ULL)|Wounded,std::memory_order_acq_rel);
                }
            }
            return TxnResult::Ok;
        }
        HierarchyLockTable& table_;std::size_t slot_,budget_;
        std::vector<Held> held_;txnid_t id_{0};std::uint64_t generation_{0},age_{0},cas_retries_{0};
        Clock::time_point deadline_;bool capacity_abort_{false};
    };
private:
    friend struct HierarchyLockTableTestAccess;
    std::atomic<std::uint8_t>& lease(std::size_t context,std::size_t node)const {
        return leases_[context*lines_+node/64].bytes[node%64];
    }
    std::size_t nodes_,contexts_;WaitPolicy policy_;std::size_t stride_,lines_;
    std::unique_ptr<Word[]> words_;std::unique_ptr<Slot[]> slots_;std::unique_ptr<LeaseLine[]> leases_;
    alignas(64) Word next_age_{0};std::atomic<bool> poisoned_{false};
};
} // namespace rrl
