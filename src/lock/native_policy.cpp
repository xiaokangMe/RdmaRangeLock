// Mechanism reference: Deneva system/txn.cpp RDMA_WAIT_DIE/RDMA_WOUND_WAIT
// and concurrency_control/rdma_maat.cpp RdmaTxnTable. Exact-width CAS,
// generation fencing and cohort poison adapt that mechanism to range slots.
// Age rules: CMU 15-445/645 (Spring 2025), Two-Phase Locking,
// slide 35; Deneva txn.cpp RDMA_WAIT_DIE / RDMA_WOUND_WAIT.
// Attempt identity and wound/commit arbitration are correctness adaptations,
// not a reproduction of a separate range-lock algorithm. See paper audit.
#include "lock/native_policy.h"
#include "lock/range_batch.h"
#include "native_txn_layout.h"
#include "rdma/rdma_transport.h"
#include "common/execution_context.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

namespace rrl {
namespace {
using Clock=std::chrono::steady_clock;
using namespace native_detail;
std::uint64_t nanos(Clock::duration d) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
}
}
struct NativeTxnMemory::Impl {
    std::vector<std::uint64_t> words;
    explicit Impl(std::size_t slots):words((sizeof(Header)+slots*sizeof(Slot))/8) {
        Header h;h.slots=slots;std::memcpy(words.data(),&h,sizeof(h));
    }
};
NativeTxnMemory::NativeTxnMemory(std::size_t slots) {
    if(!slots || slots>1048576)throw std::invalid_argument("transaction slots must be 1..1048576");
    impl_.reset(new Impl(slots));
}
NativeTxnMemory::~NativeTxnMemory()=default;
void* NativeTxnMemory::data(){return impl_->words.data();}
std::size_t NativeTxnMemory::bytes() const{return impl_->words.size()*8;}
struct NativePolicyClient::Impl {
    RdmaConnection connection;
    IRangeLockManager& physical;
    Header header;
    std::uint64_t slot{0},generation{0},age{0},last_control{0};
    txnid_t active{0};
    WaitPolicy policy{WaitPolicy::NoWait};
    Clock::time_point deadline;
    RC lock_result{RCOK};
    bool failed{false},held{false};
    std::vector<txnid_t> conflicts;
    explicit Impl(IRangeLockManager& p):physical(p){}
    bool read(std::uint64_t off,void* p,std::size_t n) {
        const auto& m=connection.remote_memory();
        if(off>m.length || n>m.length-off || !connection.read(m.address+off,m.rkey,p,n)) {
            failed=true;return false;
        }
        return true;
    }
    bool write(std::uint64_t off,const void* p,std::size_t n) {
        const auto& m=connection.remote_memory();
        if(off>m.length || n>m.length-off || !connection.write(m.address+off,m.rkey,p,n)) {
            failed=true;return false;
        }
        return true;
    }
    bool cas(std::uint64_t off,std::uint64_t expected,std::uint64_t desired,std::uint64_t& prior) {
        const auto& m=connection.remote_memory();
        if(!connection.compare_swap(m.address+off,m.rkey,expected,desired,prior)){failed=true;return false;}
        return true;
    }
    std::uint64_t offset(std::uint64_t n) const{return sizeof(Header)+n*sizeof(Slot);}
    bool healthy() {
        std::uint64_t poison=0;
        return !failed && read(offsetof(Header,poison),&poison,8) && !poison;
    }
    void poison() noexcept {
        failed=true;
        try {std::uint64_t prior=0;cas(offsetof(Header,poison),0,1,prior);} catch(...) {}
    }
    TxnResult status(txnid_t id) {
        if(!active || id!=active || !healthy())return TxnResult::Error;
        std::uint64_t word=0;
        if(!read(offset(slot),&word,8) || (word>>3)!=id)return TxnResult::Error;
        if((word&7)==Wounded)return TxnResult::Wounded;
        if((word&7)!=Running && (word&7)!=Committing)return TxnResult::Error;
        return Clock::now()>=deadline ? TxnResult::Deadline : TxnResult::Ok;
    }
    // Read identity before/after age. A stale conflict may observe a completed
    // or reused slot; it must never wound the next generation.
    bool holder(txnid_t id,Slot& out,bool& current) {
        current=false;
        if(!id || id>max_id)return false;
        const auto off=offset(id%header.slots);
        for(;;) {
            if(!read(off,&out,sizeof(out)))return false;
            if((out.control>>3)!=id || (out.control&7)==Idle)return true;
            std::uint64_t again=0;
            if(!read(off,&again,8))return false;
            if(again==out.control){current=true;return out.age!=0;}
            if(Clock::now()>=deadline)return true;
            cooperative_yield();
        }
    }
};
NativePolicyClient::NativePolicyClient(const std::string& host,std::uint16_t port,
                                       IRangeLockManager& physical):impl_(new Impl(physical)) {
    auto& p=*impl_;
    // Allocate a worker/coroutine slot once, through a probe QP. Real QP's
    // handshake token names that slot for NIC-based disconnect inspection.
    {
        RdmaConnection probe;
        if(!probe.connect_client(host,port))throw std::runtime_error("native transaction probe connect failed");
        const auto& m=probe.remote_memory();
        if(m.length<sizeof(Header) || !probe.read(m.address,m.rkey,&p.header,sizeof(Header)) ||
           p.header.signature!=magic || !p.header.slots || p.header.slots>1048576 ||
           m.length!=sizeof(Header)+p.header.slots*sizeof(Slot))
            throw std::runtime_error("native transaction layout mismatch");
        std::uint64_t n=p.header.allocated;
        for(;;) {
            if(n>=p.header.slots)throw std::runtime_error("native transaction session capacity exhausted");
            std::uint64_t prev=0;
            if(!probe.compare_swap(m.address+offsetof(Header,allocated),m.rkey,n,n+1,prev))
                throw std::runtime_error("native transaction slot allocation failed");
            if(prev==n){p.slot=n;break;}
            n=prev;
        }
    }
    p.conflicts.reserve(p.header.slots);
    if(!p.connection.connect_client(host,port,p.slot+1))
        throw std::runtime_error("native transaction session connect failed");
}
NativePolicyClient::~NativePolicyClient() {
    if(impl_->active || impl_->failed)impl_->poison();
}
PolicyReply NativePolicyClient::begin(WaitPolicy policy,std::uint64_t age,std::uint64_t budget) {
    auto& p=*impl_;
    if(p.active || policy>WaitPolicy::WoundWait || !budget || budget>600000000000ULL ||
       (age && age!=p.age) || !p.healthy())return {};
    // A dedicated service has one age policy; mixed wait graphs are invalid.
    std::uint64_t prior=0;
    if(!p.cas(offsetof(Header,policy),0,static_cast<std::uint64_t>(policy)+1,prior) ||
       (prior && prior!=static_cast<std::uint64_t>(policy)+1))return {};
    p.deadline=Clock::now()+std::chrono::nanoseconds(budget);
    if(!age) {
        if(!p.read(offsetof(Header,age),&age,8))return {};
        for(;;) {
            if(age==std::numeric_limits<std::uint64_t>::max())return {};
            if(!p.cas(offsetof(Header,age),age,age+1,prior))return {};
            if(prior==age){++age;break;}
            age=prior;
        }
    }
    if(++p.generation>(max_id-p.slot)/p.header.slots)return {};
    const auto id=p.generation*p.header.slots+p.slot;
    // Previous state is idle, and no victim can CAS it to wounded.
    if(!p.write(p.offset(p.slot)+offsetof(Slot,age),&age,8) ||
       !p.cas(p.offset(p.slot),p.last_control,control(id,Running),prior) ||
       prior!=p.last_control){p.poison();return {};}
    p.age=age;p.active=id;p.policy=policy;p.held=false;
    return {TxnResult::Ok,id,0,age};
}
PolicyReply NativePolicyClient::check(txnid_t id){return {impl_->status(id),id,0};}
PolicyReply NativePolicyClient::prepare_commit(txnid_t id) {
    auto& p=*impl_;
    const auto result=p.status(id);
    if(result!=TxnResult::Ok)return {result,id,0};
    std::uint64_t prior=0;
    // One atomic word arbitrates wound vs commit, including full attempt ID.
    if(!p.cas(p.offset(p.slot),control(id,Running),control(id,Committing),prior))return {};
    if(prior==control(id,Running))return {TxnResult::Ok,id,0};
    return {prior==control(id,Wounded)?TxnResult::Wounded:TxnResult::Error,id,0};
}
PolicyReply NativePolicyClient::end(txnid_t id) {
    auto& p=*impl_;
    if(!p.active || id!=p.active || !p.healthy())return {};
    for(;;) {
        std::uint64_t word=0,prior=0;
        if(!p.read(p.offset(p.slot),&word,8) || word>>3!=id || !(word&7) || (word&7)>Committing)return {};
        if(!p.cas(p.offset(p.slot),word,control(id,Idle),prior))return {};
        if(prior==word){p.last_control=control(id,Idle);p.active=0;return {TxnResult::Ok,id,0};}
        // The sole concurrent transition is a wound; physical slots have
        // already been released, so either outcome may now be retired.
    }
}
PolicyReply NativePolicyClient::acquire(txnid_t id,LockMode mode,const KeyRangeEnt& r,LockKind kind) {
    auto& p=*impl_;
    const auto start=Clock::now();
    p.lock_result=RCOK;
    bool waited=false;
    auto reply=[&](TxnResult result){return PolicyReply{result,id,waited?nanos(Clock::now()-start):0};};
    for(;;) {
        const auto status=p.status(id);
        if(status!=TxnResult::Ok)return reply(status);
        p.conflicts.clear();
        p.lock_result=p.physical.lock(id,mode,r,kind,&p.conflicts);
        if(p.lock_result==RCOK){p.held=true;return reply(p.status(id));}
        if(p.lock_result==AbortCapacity)return reply(TxnResult::Conflict);
        if(p.lock_result==AbortGate)return reply(TxnResult::Conflict);
        if(p.lock_result!=Abort && p.lock_result!=AbortConflict)return reply(TxnResult::Error);
        if(p.policy==WaitPolicy::NoWait)return reply(TxnResult::Conflict);
        if(p.conflicts.empty())return reply(TxnResult::Error);
        for(const auto other:p.conflicts) {
            Slot holder{};bool current=false;
            if(!p.holder(other,holder,current))return reply(TxnResult::Error);
            if(!current)continue;
            if(p.policy==WaitPolicy::WaitDie && p.age>holder.age)return reply(TxnResult::Conflict);
            if(p.policy==WaitPolicy::WoundWait && p.age<holder.age && (holder.control&7)==Running) {
                std::uint64_t prior=0;
                if(!p.cas(p.offset(other%p.header.slots),holder.control,control(other,Wounded),prior))
                    return reply(TxnResult::Error);
            }
        }
        // Deneva retries the real physical CAS after giving holders a chance
        // to finish; no second metadata grant, no sleep/backoff, finite budget.
        waited=true;cooperative_yield();
    }
}
PolicyReply NativePolicyClient::acquire_batch(txnid_t id, LockMode mode,
                                                   const std::vector<KeyRangeEnt>& ranges,
                                                   LockKind kind) {
    auto& p=*impl_;p.lock_result=RCOK;const auto start=Clock::now();bool waited=false;
    auto reply=[&](TxnResult result){return PolicyReply{result,id,waited?nanos(Clock::now()-start):0};};
    if (ranges.empty() || mode > LockMode::X) return reply(TxnResult::Error);
    for (const auto& range : ranges) if (range.empty() || !range.valid()) return reply(TxnResult::Error);
    for (;;) {
        const auto status=p.status(id);if(status!=TxnResult::Ok)return reply(status);
        p.conflicts.clear();
        RC rc=ERROR;
        if (auto* batch=dynamic_cast<IRangeLockBatch*>(&p.physical))
            rc=batch->lock_batch(id,mode,ranges,kind,&p.conflicts);
        else {
            rc=RCOK;
            for (const auto& range:ranges) {
                p.conflicts.clear();rc=p.physical.lock(id,mode,range,kind,&p.conflicts);p.lock_result=rc;
                if (rc!=RCOK) break;
                p.held=true;
                const auto status=p.status(id);
                if(status!=TxnResult::Ok)return reply(status);
            }
        }
        p.lock_result=rc;
        if(rc==RCOK){p.held=true;return reply(p.status(id));}
        if(rc==AbortCapacity || rc==AbortGate)return reply(TxnResult::Conflict);
        if(rc!=Abort && rc!=AbortConflict)return reply(TxnResult::Error);
        if(p.policy==WaitPolicy::NoWait)return reply(TxnResult::Conflict);
        if(p.conflicts.empty())return reply(TxnResult::Error);
        for(const auto other:p.conflicts) {
            Slot holder{};bool current=false;
            if(!p.holder(other,holder,current))return reply(TxnResult::Error);
            if(!current)continue;
            if(p.policy==WaitPolicy::WaitDie && p.age>holder.age)return reply(TxnResult::Conflict);
            if(p.policy==WaitPolicy::WoundWait && p.age<holder.age && (holder.control&7)==Running) {
                std::uint64_t prior=0;
                if(!p.cas(p.offset(other%p.header.slots),holder.control,control(other,Wounded),prior)) {
                    const auto observed=p.status(other);
                    if(observed==TxnResult::Ok || observed==TxnResult::Wounded)continue;
                    return reply(TxnResult::Error);
                }
            }
        }
        waited=true;cooperative_yield();
    }
}

std::uint64_t NativePolicyClient::rdma_operations() const{return impl_->connection.operation_count();}
bool NativePolicyClient::has_physical_locks() const{return impl_->held;}
RC NativePolicyClient::last_lock_result() const{return impl_->lock_result;}

struct LocalNativePolicyState::Impl {
    struct AtomicSlot {
        std::atomic<std::uint64_t> control{0};
        std::atomic<std::uint64_t> age{0};
    };
    const std::size_t slots;
    std::unique_ptr<AtomicSlot[]> table;
    std::atomic<std::uint64_t> allocated{0};
    std::atomic<std::uint64_t> next_age{0};
    std::atomic<std::uint64_t> policy{0};
    std::atomic<std::uint64_t> poison{0};
    explicit Impl(std::size_t n):slots(n),table(new AtomicSlot[n]){}
};

LocalNativePolicyState::LocalNativePolicyState(std::size_t slots) {
    if(!slots || slots>1048576)throw std::invalid_argument("local transaction slots must be 1..1048576");
    impl_.reset(new Impl(slots));
}
LocalNativePolicyState::~LocalNativePolicyState()=default;

struct LocalNativePolicyClient::Impl {
    LocalNativePolicyState::Impl& state;
    IRangeLockManager& physical;
    std::size_t slot{0};
    std::uint64_t generation{0},age{0},last_control{0},active{0};
    WaitPolicy policy{WaitPolicy::NoWait};
    Clock::time_point deadline;
    bool failed{false},held{false};
    std::vector<txnid_t> conflicts;
    Impl(LocalNativePolicyState::Impl& s,IRangeLockManager& p):state(s),physical(p) {
        const auto n=state.allocated.fetch_add(1,std::memory_order_acq_rel);
        if(n>=state.slots)throw std::runtime_error("local transaction session capacity exhausted");
        slot=static_cast<std::size_t>(n);
        conflicts.reserve(state.slots);
    }
    static std::uint64_t control(txnid_t id,native_detail::State state) {
        return native_detail::control(id,state);
    }
    bool healthy() const {
        return !failed && state.poison.load(std::memory_order_acquire)==0;
    }
    void poison() noexcept {
        failed=true;
        state.poison.store(1,std::memory_order_release);
    }
    std::uint64_t offset_slot(txnid_t id) const {return id%state.slots;}
    TxnResult status(txnid_t id) const {
        if(!active || id!=active || !healthy())return TxnResult::Error;
        const auto word=state.table[slot].control.load(std::memory_order_acquire);
        if((word>>3)!=id)return TxnResult::Error;
        const auto status=word&7;
        if(status==native_detail::Wounded)return TxnResult::Wounded;
        if(status!=native_detail::Running && status!=native_detail::Committing)return TxnResult::Error;
        return Clock::now()>=deadline?TxnResult::Deadline:TxnResult::Ok;
    }
    bool holder(txnid_t id,std::uint64_t& holder_age,std::uint64_t& word,bool& current) const {
        current=false;holder_age=0;word=0;
        if(!id || id>native_detail::max_id)return false;
        auto& slot_ref=state.table[offset_slot(id)];
        for(;;) {
            const auto first=slot_ref.control.load(std::memory_order_acquire);
            const auto observed_age=slot_ref.age.load(std::memory_order_acquire);
            const auto second=slot_ref.control.load(std::memory_order_acquire);
            if(first!=second) {
                if(Clock::now()>=deadline)return true;
                cooperative_yield();continue;
            }
            word=first;holder_age=observed_age;
            if((first>>3)!=id || (first&7)==native_detail::Idle)return true;
            current=true;return observed_age!=0;
        }
    }
};

LocalNativePolicyClient::LocalNativePolicyClient(LocalNativePolicyState& state,IRangeLockManager& physical)
    :impl_(new Impl(*state.impl_,physical)){}
LocalNativePolicyClient::~LocalNativePolicyClient() {
    if(impl_->active || impl_->failed)impl_->poison();
}
PolicyReply LocalNativePolicyClient::begin(WaitPolicy policy,std::uint64_t age,std::uint64_t budget_ns) {
    auto& p=*impl_;
    if(p.active || policy>WaitPolicy::WoundWait || !budget_ns || budget_ns>600000000000ULL ||
       (age && age!=p.age) || !p.healthy())return {};
    std::uint64_t expected=0;
    const auto wanted=static_cast<std::uint64_t>(policy)+1;
    if(!p.state.policy.compare_exchange_strong(expected,wanted,std::memory_order_acq_rel) && expected!=wanted)return {};
    p.deadline=Clock::now()+std::chrono::nanoseconds(budget_ns);
    if(!age)age=p.state.next_age.fetch_add(1,std::memory_order_acq_rel)+1;
    if(!age || age==std::numeric_limits<std::uint64_t>::max())return {};
    if(++p.generation>(native_detail::max_id-p.slot)/p.state.slots)return {};
    const auto id=p.generation*p.state.slots+p.slot;
    auto& slot=p.state.table[p.slot];
    slot.age.store(age,std::memory_order_release);
    expected=p.last_control;
    const auto desired=Impl::control(id,native_detail::Running);
    if(!slot.control.compare_exchange_strong(expected,desired,std::memory_order_acq_rel)) {
        p.poison();return {};
    }
    p.age=age;p.active=id;p.policy=policy;p.held=false;
    return {TxnResult::Ok,id,0,age};
}
PolicyReply LocalNativePolicyClient::check(txnid_t id) {return {impl_->status(id),id,0};}
PolicyReply LocalNativePolicyClient::prepare_commit(txnid_t id) {
    auto& p=*impl_;const auto result=p.status(id);
    if(result!=TxnResult::Ok)return {result,id,0};
    auto& slot=p.state.table[p.slot];auto expected=Impl::control(id,native_detail::Running);
    if(slot.control.compare_exchange_strong(expected,Impl::control(id,native_detail::Committing),std::memory_order_acq_rel))
        return {TxnResult::Ok,id,0};
    return {expected==Impl::control(id,native_detail::Wounded)?TxnResult::Wounded:TxnResult::Error,id,0};
}
PolicyReply LocalNativePolicyClient::end(txnid_t id) {
    auto& p=*impl_;
    if(!p.active || id!=p.active || !p.healthy())return {};
    auto& slot=p.state.table[p.slot];
    for(;;) {
        auto word=slot.control.load(std::memory_order_acquire);
        if((word>>3)!=id || !(word&7) || (word&7)>native_detail::Committing)return {};
        if(slot.control.compare_exchange_weak(word,Impl::control(id,native_detail::Idle),std::memory_order_acq_rel)) {
            p.last_control=Impl::control(id,native_detail::Idle);p.active=0;p.held=false;
            return {TxnResult::Ok,id,0};
        }
    }
}
bool LocalNativePolicyClient::has_physical_locks() const {return impl_->held;}

PolicyReply LocalNativePolicyClient::acquire(txnid_t id,LockMode mode,const KeyRangeEnt& range,LockKind kind) {
    auto& p=*impl_;const auto start=Clock::now();bool waited=false;
    auto reply=[&](TxnResult result){return PolicyReply{result,id,waited?nanos(Clock::now()-start):0};};
    for(;;) {
        const auto status=p.status(id);if(status!=TxnResult::Ok)return reply(status);
        p.conflicts.clear();
        const auto rc=p.physical.lock(id,mode,range,kind,&p.conflicts);
        if(rc==RCOK){p.held=true;return reply(p.status(id));}
        if(rc==AbortCapacity || rc==AbortGate || p.policy==WaitPolicy::NoWait)return reply(TxnResult::Conflict);
        if(rc!=Abort && rc!=AbortConflict)return reply(TxnResult::Error);
        if(p.conflicts.empty())return reply(TxnResult::Error);
        for(const auto other:p.conflicts) {
            std::uint64_t holder_age=0,word=0;bool current=false;
            if(!p.holder(other,holder_age,word,current))return reply(TxnResult::Error);
            if(!current)continue;
            if(p.policy==WaitPolicy::WaitDie && p.age>holder_age)return reply(TxnResult::Conflict);
            if(p.policy==WaitPolicy::WoundWait && p.age<holder_age && (word&7)==native_detail::Running) {
                auto expected=word;
                if(!p.state.table[p.offset_slot(other)].control.compare_exchange_strong(expected,Impl::control(other,native_detail::Wounded),std::memory_order_acq_rel)) {
                    const auto observed=p.state.table[p.offset_slot(other)].control.load(std::memory_order_acquire);
                    if((observed>>3)==other && (observed&7)!=native_detail::Idle)continue;
                    return reply(TxnResult::Error);
                }
            }
        }
        waited=true;cooperative_yield();
    }
}

PolicyReply LocalNativePolicyClient::acquire_batch(txnid_t id, LockMode mode,
                                                        const std::vector<KeyRangeEnt>& ranges,
                                                        LockKind kind) {
    auto& p=*impl_;const auto start=Clock::now();bool waited=false;
    auto reply=[&](TxnResult result){return PolicyReply{result,id,waited?nanos(Clock::now()-start):0};};
    if (ranges.empty() || mode > LockMode::X) return reply(TxnResult::Error);
    for (const auto& range : ranges) if (range.empty() || !range.valid()) return reply(TxnResult::Error);
    for (;;) {
        const auto status=p.status(id);if(status!=TxnResult::Ok)return reply(status);
        p.conflicts.clear();RC rc=ERROR;
        if (auto* batch=dynamic_cast<IRangeLockBatch*>(&p.physical))
            rc=batch->lock_batch(id,mode,ranges,kind,&p.conflicts);
        else {
            rc=RCOK;
            for (const auto& range:ranges) {
                p.conflicts.clear();rc=p.physical.lock(id,mode,range,kind,&p.conflicts);
                if (rc!=RCOK) break;
                p.held=true;
                const auto status=p.status(id);
                if(status!=TxnResult::Ok)return reply(status);
            }
        }
        if(rc==RCOK){p.held=true;return reply(p.status(id));}
        if(rc==AbortCapacity || rc==AbortGate || p.policy==WaitPolicy::NoWait)return reply(TxnResult::Conflict);
        if(rc!=Abort && rc!=AbortConflict)return reply(TxnResult::Error);
        if(p.conflicts.empty())return reply(TxnResult::Error);
        for(const auto other:p.conflicts) {
            std::uint64_t holder_age=0,word=0;bool current=false;
            if(!p.holder(other,holder_age,word,current))return reply(TxnResult::Error);
            if(!current)continue;
            if(p.policy==WaitPolicy::WaitDie && p.age>holder_age)return reply(TxnResult::Conflict);
            if(p.policy==WaitPolicy::WoundWait && p.age<holder_age && (word&7)==native_detail::Running) {
                auto expected=word;
                if(!p.state.table[p.offset_slot(other)].control.compare_exchange_strong(expected,Impl::control(other,native_detail::Wounded),std::memory_order_acq_rel)) {
                    const auto observed=p.state.table[p.offset_slot(other)].control.load(std::memory_order_acquire);
                    if((observed>>3)==other && (observed&7)!=native_detail::Idle)continue;
                    return reply(TxnResult::Error);
                }
            }
        }
        waited=true;cooperative_yield();
    }
}

int run_native_txn_server(std::uint16_t port,std::size_t slots) {
    NativeTxnMemory memory(slots);
    RdmaListener listener(port);
    if(!listener.valid())throw std::runtime_error("native txn listener failed");
    std::cout<<"RDMA transaction state server listening on port "<<port<<",slots="<<slots<<std::endl;
    for(;;) {
        auto c=std::make_unique<RdmaConnection>();
        if(!c->accept_server(listener,memory.data(),memory.bytes())) {
            if(c->rejected_handshake())continue;
            throw std::runtime_error("native txn accept failed");
        }
        std::thread([c=std::move(c),port,slots]() mutable {
            const auto owner=c->peer_owner_token();
            c->wait_for_disconnect();
            c.reset(); // destroy original QP before inspecting its writes
            if(!owner)return; // allocation/recovery probe, never executes a txn
            RdmaConnection recovery;
            const auto deadline=Clock::now()+std::chrono::seconds(10);
            recovery.set_deadline(deadline);
            if(owner>slots || !recovery.connect_client("127.0.0.1",port))_exit(3);
            const auto& m=recovery.remote_memory();
            std::uint64_t word=0;
            if(!recovery.read(m.address+sizeof(Header)+(owner-1)*sizeof(Slot),m.rkey,&word,8))_exit(3);
            if(word&7) {
                // Passive memory is observed/poisoned via NIC, never CPU atomics
                // racing against NIC CAS. Never clear a victim's actual locks.
                std::uint64_t prior=0;
                if(!recovery.compare_swap(m.address+offsetof(Header,poison),m.rkey,0,1,prior))_exit(3);
                std::cerr<<"native txn cohort poisoned after active session disconnect"<<std::endl;
            }
        }).detach();
    }
}
} // namespace rrl
