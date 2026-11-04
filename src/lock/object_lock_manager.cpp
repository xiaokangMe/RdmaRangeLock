// Deneva row-head short CAS + holder metadata, adapted to stable index IDs.
#include "lock/object_lock_manager.h"
#include "object_lock_layout.h"
#include "rdma/rdma_transport.h"
#include "common/execution_context.h"
#include "common/fifo_mutex.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <unistd.h>
namespace rrl {
using namespace object_detail;
namespace {
using Clock=std::chrono::steady_clock;
struct IO {
    virtual ~IO()=default;
    virtual bool read(std::uint64_t,void*,std::size_t)=0;
    virtual bool write(std::uint64_t,const void*,std::size_t)=0;
    virtual bool cas(std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t&)=0;
};
struct Image final:IO {
    std::vector<std::uint64_t> words;
    explicit Image(const Header& h):words(h.bytes/8){std::memcpy(words.data(),&h,sizeof(h));}
    bool read(std::uint64_t off,void* p,std::size_t n) override {
        if(off>words.size()*8 || n>words.size()*8-off)return false;
        if(off%8 || n%8)return false;
        for(std::size_t i=0;i<n;i+=8) {
            const auto value=__atomic_load_n(words.data()+(off+i)/8,__ATOMIC_SEQ_CST);
            std::memcpy(static_cast<char*>(p)+i,&value,8);
        }
        return true;
    }
    bool write(std::uint64_t off,const void* p,std::size_t n) override {
        if(off>words.size()*8 || n>words.size()*8-off)return false;
        if(off%8 || n%8)return false;
        for(std::size_t i=0;i<n;i+=8) {
            std::uint64_t value;std::memcpy(&value,static_cast<const char*>(p)+i,8);
            __atomic_store_n(words.data()+(off+i)/8,value,__ATOMIC_SEQ_CST);
        }
        return true;
    }
    bool cas(std::uint64_t off,std::uint64_t expected,std::uint64_t desired,std::uint64_t& prior) override {
        if(off%8 || off>words.size()*8 || 8>words.size()*8-off)return false;
        prior=expected;__atomic_compare_exchange_n(words.data()+off/8,&prior,desired,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return true;
    }
};
struct Remote final:IO {
    RdmaConnection connection;
    bool read(std::uint64_t off,void* p,std::size_t n) override {
        const auto m=connection.remote_memory();return off<=m.length && n<=m.length-off && connection.read(m.address+off,m.rkey,p,n);
    }
    bool write(std::uint64_t off,const void* p,std::size_t n) override {
        const auto m=connection.remote_memory();return off<=m.length && n<=m.length-off && connection.write(m.address+off,m.rkey,p,n);
    }
    bool cas(std::uint64_t off,std::uint64_t expected,std::uint64_t desired,std::uint64_t& prior) override {
        const auto m=connection.remote_memory();return off<=m.length && 8<=m.length-off && connection.compare_swap(m.address+off,m.rkey,expected,desired,prior);
    }
};
struct Held {std::uint64_t object,node;txnid_t txn;LockMode mode;};
struct Context {
    std::unique_ptr<Remote> remote;
    IO* io{nullptr};
    std::uint64_t slot{0};
    FifoMutex mutex;
    std::vector<Held> held;
    std::vector<std::size_t> free;
};
}
struct ObjectLockManager::Impl {
    Header header;
    std::vector<idx_key_t> records;
    std::unique_ptr<Image> image;
    std::vector<std::unique_ptr<Context>> contexts;
    mutable std::atomic<bool> failed{false};
    Context& context() const {return *contexts[execution_context_id()%contexts.size()];}
    void setup(Context& c) {
        c.held.reserve(header.budget);c.free.reserve(header.budget);
        for(std::size_t n=header.budget;n;--n)c.free.push_back(n-1);
    }
    std::uint64_t state(const Context& c) const{return header.session_base+c.slot*sizeof(object_detail::Session);}
    bool active(Context& c,bool value) {
        object_detail::Session s{value?1ULL:0ULL,c.held.size()};
        if(!c.io->write(state(c),&s,sizeof(s))){failed=true;return false;}return true;
    }
    std::uint64_t object(const KeyRangeEnt& range,LockKind kind) const {
        if(!range.has_start || !range.has_end || range.StartKey!=range.EndKey ||
           range.start_bound!=Bound::Closed || range.end_bound!=Bound::Closed)throw std::invalid_argument("object locks require exact stable IDs");
        std::uint64_t id=range.StartKey;
        if(kind==LockKind::Record) {
            const auto it=std::lower_bound(records.begin(),records.end(),id);
            if(it==records.end() || *it!=id)throw std::invalid_argument("unregistered record resource");
            id=header.nodes+std::uint64_t(it-records.begin());
        }else if(kind!=LockKind::Node || id>=header.nodes)throw std::invalid_argument("unregistered leaf resource");
        return header.object_base+id*sizeof(Object);
    }
    void enter(Context& c,std::uint64_t off) {
        const auto deadline=Clock::now()+std::chrono::seconds(10);
        for(;;) {
            std::uint64_t prior=0;
            if(!c.io->cas(off,0,c.slot+1,prior))throw std::runtime_error("object metadata CAS failed");
            if(!prior)return;
            if(prior==c.slot+1)throw std::runtime_error("recursive object gate");
            if(Clock::now()>=deadline)throw std::runtime_error("object metadata latch deadline");
            cooperative_yield();
        }
    }
    void leave(Context& c,std::uint64_t off) {
        std::uint64_t prior=0;
        if(!c.io->cas(off,c.slot+1,0,prior) || prior!=c.slot+1)throw std::runtime_error("object metadata release failed");
    }
    Holder holder(Context& c,std::uint64_t off) const {
        if(off<header.holder_base || (off-header.holder_base)%sizeof(Holder) || off>header.bytes || sizeof(Holder)>header.bytes-off)
            throw std::runtime_error("invalid holder link");
        Holder h;
        if(!c.io->read(off,&h,sizeof(h)) || !h.txn || h.mode>3)throw std::runtime_error("invalid holder");return h;
    }
    void poison(Context& c) noexcept {
        failed=true;std::uint64_t prior=0;
        try{c.io->cas(offsetof(Header,poison),0,1,prior);}catch(...){}
    }
};
ObjectLockManager::ObjectLockManager():impl_(new Impl){}
ObjectLockManager::~ObjectLockManager()=default;
bool ObjectLockManager::init_local(const std::vector<idx_key_t>& records,std::size_t nodes,std::size_t contexts,std::size_t budget) {
    auto& p=*impl_;
    try {
        if(!p.contexts.empty() || !std::is_sorted(records.begin(),records.end()) || std::adjacent_find(records.begin(),records.end())!=records.end())return false;
        p.records=records;p.header=layout(records.size(),nodes,contexts,budget,fingerprint(records));p.image.reset(new Image(p.header));
        for(std::size_t n=0;n<contexts;++n){auto c=std::make_unique<Context>();c->slot=n;c->io=p.image.get();p.setup(*c);p.contexts.push_back(std::move(c));}
        return true;
    }catch(...){p.failed=true;return false;}
}
bool ObjectLockManager::connect(const std::string& host,std::uint16_t port,const std::vector<idx_key_t>& records,std::size_t contexts) {
    auto& p=*impl_;
    try {
        if(!contexts || !p.contexts.empty() || !std::is_sorted(records.begin(),records.end()) || std::adjacent_find(records.begin(),records.end())!=records.end())return false;
        p.records=records;Remote probe;
        if(!probe.connection.connect_client(host,port) || !probe.read(0,&p.header,sizeof(Header)))return false;
        const auto h=p.header;
        const auto expected=layout(h.records,h.nodes,h.sessions,h.budget,h.fingerprint);
        if(h.signature!=magic || h.poison || h.bytes!=expected.bytes || h.object_base!=expected.object_base ||
           h.holder_base!=expected.holder_base || h.session_base!=sizeof(Header) || h.bytes!=probe.connection.remote_memory().length ||
           h.records!=records.size() || h.fingerprint!=fingerprint(records) || contexts>h.sessions)return false;
        for(std::size_t n=0;n<contexts;++n) {
            std::uint64_t slot=0,prior=0;
            if(!probe.read(offsetof(Header,allocated),&slot,8))return false;
            for(;;){if(slot>=h.sessions)return false;
                if(!probe.cas(offsetof(Header,allocated),slot,slot+1,prior))return false;
                if(prior==slot)break;slot=prior;}
            auto c=std::make_unique<Context>();c->slot=slot;c->remote.reset(new Remote);c->io=c->remote.get();
            if(!c->remote->connection.connect_client(host,port,slot+1))return false;
            p.setup(*c);p.contexts.push_back(std::move(c));
        }
        return true;
    }catch(...){p.failed=true;return false;}
}
RC ObjectLockManager::lock(txnid_t txn,LockMode mode,const KeyRangeEnt& range,LockKind kind,std::vector<txnid_t>* conflicts) {
    auto& p=*impl_;if(p.failed || !txn || mode>LockMode::X || p.contexts.empty())return ERROR;
    auto& c=p.context();std::lock_guard<FifoMutex> guard(c.mutex);
    try {
        std::uint64_t poison=0;
        if(!c.io->read(offsetof(Header,poison),&poison,8) || poison){p.failed=true;return ERROR;}
        const auto off=p.object(range,kind);
        auto owned=std::find_if(c.held.begin(),c.held.end(),[&](const Held& h){return h.txn==txn && h.object==off;});
        if(owned!=c.held.end()) {const auto joined=lock_mode_join(owned->mode,mode);if(joined==owned->mode)return RCOK;mode=joined;}
        if(owned==c.held.end() && c.free.empty())return AbortCapacity;
        if(!p.active(c,true))return ERROR;
        p.enter(c,off);std::uint64_t head=0;
        if(!c.io->read(off+offsetof(Object,head),&head,8))throw std::runtime_error("object head read");
        bool conflict=false;std::size_t seen=0;
        for(auto link=head;link;) {
            if(++seen>p.header.sessions*p.header.budget)throw std::runtime_error("holder cycle");
            const auto h=p.holder(c,link);
            if(h.txn!=txn && lock_modes_conflict(mode,static_cast<LockMode>(h.mode))) {
                conflict=true;if(conflicts && std::find(conflicts->begin(),conflicts->end(),h.txn)==conflicts->end())conflicts->push_back(h.txn);
            }
            link=h.next;
        }
        if(conflict){p.leave(c,off);if(!p.active(c,!c.held.empty()))return ERROR;return AbortConflict;}
        if(owned!=c.held.end()) {
            const auto value=static_cast<std::uint64_t>(mode);
            if(!c.io->write(owned->node+offsetof(Holder,mode),&value,8))throw std::runtime_error("holder upgrade");owned->mode=mode;
        }else {
            const auto node=p.header.holder_base+(c.slot*p.header.budget+c.free.back())*sizeof(Holder);
            Holder h{txn,static_cast<std::uint64_t>(mode),head};
            if(!c.io->write(node,&h,sizeof(h)) || !c.io->write(off+offsetof(Object,head),&node,8))throw std::runtime_error("holder publication");
            c.free.pop_back();c.held.push_back({off,node,txn,mode});
        }
        p.leave(c,off);if(!p.active(c,true))return ERROR;return RCOK;
    }catch(...){p.poison(c);return ERROR;}
}
RC ObjectLockManager::unlock(txnid_t txn,const KeyRangeEnt& range,LockKind kind) {
    auto& p=*impl_;if(p.failed || p.contexts.empty())return ERROR;
    auto& c=p.context();std::lock_guard<FifoMutex> guard(c.mutex);
    try {
        const auto off=p.object(range,kind);
        auto found=std::find_if(c.held.begin(),c.held.end(),[&](const Held& h){return h.txn==txn && h.object==off;});
        if(found==c.held.end())return Abort;
        if(!p.active(c,true))return ERROR;
        p.enter(c,off);std::uint64_t link=0,previous=0;
        if(!c.io->read(off+offsetof(Object,head),&link,8))throw std::runtime_error("release head");
        std::size_t seen=0;
        while(link && link!=found->node) {
            if(++seen>p.header.sessions*p.header.budget)throw std::runtime_error("release cycle");
            previous=link;link=p.holder(c,link).next;
        }
        if(!link)throw std::runtime_error("lost physical holder");
        const auto h=p.holder(c,link);if(h.txn!=txn)throw std::runtime_error("holder identity changed");
        const auto target=previous?previous+offsetof(Holder,next):off+offsetof(Object,head);
        if(!c.io->write(target,&h.next,8))throw std::runtime_error("release link");
        const std::uint64_t empty=0;
        if(!c.io->write(link,&empty,8))throw std::runtime_error("retire holder");
        p.leave(c,off);
        c.free.push_back((found->node-p.header.holder_base)/sizeof(Holder)-c.slot*p.header.budget);
        c.held.erase(found);if(!p.active(c,!c.held.empty()))return ERROR;return RCOK;
    }catch(...){p.poison(c);return ERROR;}
}
RC ObjectLockManager::unlock_all(txnid_t txn) {
    auto& p=*impl_;if(p.failed || p.contexts.empty())return ERROR;
    // Every transaction is bound to its execution context for its full life.
    auto& c=p.context();bool found=false;
    for(;;) {
        std::uint64_t object=0;
        {std::lock_guard<FifoMutex> guard(c.mutex);
         auto it=std::find_if(c.held.rbegin(),c.held.rend(),[&](const Held& h){return h.txn==txn;});
         if(it==c.held.rend())break;object=it->object;}
        const auto index=(object-p.header.object_base)/sizeof(Object);
        const bool record=index>=p.header.nodes;
        const auto key=record?p.records[index-p.header.nodes]:index;
        if(unlock(txn,KeyRangeEnt::point(key),record?LockKind::Record:LockKind::Node)!=RCOK)return ERROR;found=true;
    }
    return found?RCOK:Abort;
}
std::size_t ObjectLockManager::get_lock_count(txnid_t txn) const {
    auto& p=*impl_;if(p.failed || p.contexts.empty())throw std::runtime_error("object count unavailable");
    auto& c=p.context();std::lock_guard<FifoMutex> guard(c.mutex);
    std::uint64_t allocated=0;
    if(!c.io->read(offsetof(Header,allocated),&allocated,8))throw std::runtime_error("object session count read");
    if(p.image)allocated=p.header.sessions;
    if(allocated>p.header.sessions)throw std::runtime_error("invalid allocated sessions");
    std::size_t count=0;
    for(std::uint64_t slot=0;slot<allocated;++slot) {
        object_detail::Session state;
        if(!c.io->read(p.header.session_base+slot*sizeof(state),&state,sizeof(state)))throw std::runtime_error("object session read");
        if(!txn){count+=state.count;continue;}
        // Diagnostic snapshot, exact once the cohort is quiescent. No scan
        // of the key/leaf object directory, even at the two-million scale.
        if(!state.count)continue;
        Holder buffer[128];
        for(std::uint64_t i=0;i<p.header.budget;i+=128) {
            const auto n=std::min<std::uint64_t>(128,p.header.budget-i);
            if(!c.io->read(p.header.holder_base+(slot*p.header.budget+i)*sizeof(Holder),buffer,n*sizeof(Holder)))throw std::runtime_error("holder count read");
            for(std::uint64_t j=0;j<n;++j)if(buffer[j].txn==txn)++count;
        }
    }
    return count;
}
std::uint64_t ObjectLockManager::rdma_operations() const {std::uint64_t n=0;for(auto& c:impl_->contexts)if(c->remote)n+=c->remote->connection.operation_count();return n;}
std::uint64_t ObjectLockManager::rdma_timeouts() const {std::uint64_t n=0;for(auto& c:impl_->contexts)if(c->remote)n+=c->remote->connection.timeout_count();return n;}
std::uint64_t ObjectLockManager::rdma_completion_errors() const {std::uint64_t n=0;for(auto& c:impl_->contexts)if(c->remote)n+=c->remote->connection.completion_error_count();return n;}
int run_object_lock_server(std::uint16_t port,const std::vector<idx_key_t>& records,std::size_t nodes,std::size_t sessions,std::size_t budget) {
    const auto h=layout(records.size(),nodes,sessions,budget,fingerprint(records));Image image(h);
    RdmaListener listener(port);if(!listener.valid())throw std::runtime_error("object listener");
    std::cout<<"RDMA object lock server listening on port "<<port<<",objects="<<records.size()+nodes<<",bytes="<<h.bytes<<std::endl;
    for(;;) {
        auto* connection=new RdmaConnection;
        if(!connection->accept_server(listener,image.words.data(),h.bytes)) {
            const bool rejected=connection->rejected_handshake();delete connection;
            if(rejected)continue;throw std::runtime_error("object accept");
        }
        std::thread([connection,port,h] {
            connection->wait_for_disconnect();const auto owner=connection->peer_owner_token();delete connection;
            if(!owner)return;
            Remote recovery;
            if(owner>h.sessions || !recovery.connection.connect_client("127.0.0.1",port))_exit(2);
            object_detail::Session state;
            if(!recovery.read(h.session_base+(owner-1)*sizeof(state),&state,sizeof(state)) || state.active || state.count) {
                std::cerr<<"native object owner disconnected active; fail closed"<<std::endl;_exit(2);
            }
        }).detach();
    }
}
} // namespace rrl
