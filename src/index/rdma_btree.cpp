#include "index/rdma_btree.h"
#include "rdma/rdma_transport.h"
#include "rdma_btree_core.h"
#include <atomic>
#include <chrono>
#include <thread>

namespace rrl {
using namespace detail;
namespace {
using Clock=std::chrono::steady_clock;
struct ImageIO final : TreeIO {
    std::vector<std::uint64_t> words;
    explicit ImageIO(std::size_t bytes):words(bytes/8){}
    bool read(std::uint64_t off,void* p,std::size_t n) override {
        if(off>words.size()*8 || n>words.size()*8-off)return false;
        std::memcpy(p,reinterpret_cast<const char*>(words.data())+off,n);return true;
    }
    bool write(std::uint64_t off,const void* p,std::size_t n) override {
        if(off>words.size()*8 || n>words.size()*8-off)return false;
        std::memcpy(reinterpret_cast<char*>(words.data())+off,p,n);return true;
    }
};
struct RemoteIO final : TreeIO {
    RdmaConnection connection;
    std::uint64_t reads{0},writes{0};
    bool read(std::uint64_t off,void* p,std::size_t n) override {
        const auto& m=connection.remote_memory();
        if(off>m.length || n>m.length-off)return false;
        if(!connection.read(m.address+off,m.rkey,p,n))return false;
        reads+=n;return true;
    }
    bool write(std::uint64_t off,const void* p,std::size_t n) override {
        const auto& m=connection.remote_memory();
        if(off>m.length || n>m.length-off)return false;
        if(!connection.write(m.address+off,m.rkey,p,n))return false;
        writes+=n;return true;
    }
    bool cas(std::uint64_t compare,std::uint64_t swap,std::uint64_t& previous) {
        const auto& m=connection.remote_memory();
        return connection.compare_swap(m.address,m.rkey,compare,swap,previous);
    }
};
struct Session {
    RemoteIO io;
    std::unique_ptr<TreeCore> core;
    std::vector<std::uint64_t> values;
    std::uint64_t point_value{0},wait_ns{0};
    bool attached{false};
};
thread_local const void* attached_tree=nullptr;
thread_local Session* attached_session=nullptr;
}
std::uint64_t tree_seed_fingerprint(const TreeSeed& seed) {
    std::uint64_t hash=1469598103934665603ULL;
    auto word=[&](std::uint64_t v){for(unsigned n=0;n<8;++n){hash^=(v&255);hash*=1099511628211ULL;v>>=8;}};
    word(seed.size());for(const auto& p:seed){word(p.first);word(p.second);}return hash;
}
struct RdmaTreeMemory::Impl {
    ImageIO io;
    explicit Impl(const RdmaTreeConfig& c,const TreeSeed& seed):io(make_header(c).bytes) {
        auto h=make_header(c);h.seed_fingerprint=tree_seed_fingerprint(seed);
        io.write(0,&h,sizeof(h));TreeCore tree(io,h);
        tree.path[0].head=OffsetBTreePage{};tree.write_node(h.root,tree.path[0]);
        idx_key_t prev=0;bool first=true;
        for(const auto& p:seed) {
            if(!first && p.first<=prev)throw TreeFault("seed must be sorted and unique");
            first=false;prev=p.first;tree.mutate(IndexMutation{p.first,&p.second,false});
        }
        tree.publish();
    }
};
RdmaTreeMemory::RdmaTreeMemory(const RdmaTreeConfig& c,const TreeSeed& seed):impl_(new Impl(c,seed)){}
RdmaTreeMemory::~RdmaTreeMemory()=default;
void* RdmaTreeMemory::data(){return impl_->io.words.data();}
std::size_t RdmaTreeMemory::bytes() const{return impl_->io.words.size()*8;}

struct BTreeRdma::Impl {
    std::vector<std::unique_ptr<Session>> workers;
    mutable Session admin;
    OffsetBTreeHeader initial;
    std::uint64_t timeout_ms{10000};
    mutable std::atomic<bool> failed{false};
    Session& session() const {
        if(attached_tree==this && attached_session)return *attached_session;
        throw TreeFault("RDMA index worker not prepared before Begin");
    }
    void poison(Session& s) const noexcept {
        failed=true;const std::uint64_t one=1;
        s.io.write(offsetof(OffsetBTreeHeader,poison),&one,sizeof(one));
    }
    void enter(Session& s,bool writer) const {
        if(failed)throw TreeFault("shared data backend failed");
        const auto start=Clock::now(),deadline=start+std::chrono::milliseconds(timeout_ms);
        // This is bounded structural-latch acquisition, not a transaction
        // conflict retry. No sleep/backoff; network faults stop the cohort.
        std::uint64_t current=0;
        bool announced=false;
        for(;;) {
            if(writer && announced) {
                if(current==writer_bit)break;
                if(!s.io.read(0,&current,sizeof(current))){failed=true;throw TreeFault("writer drain read failed");}
            } else if(!(current&writer_bit)) {
                const auto desired=writer ? (current|writer_bit) : current+1;
                std::uint64_t previous=0;
                if(!s.io.cas(current,desired,previous)){failed=true;throw TreeFault("structure latch CAS failed");}
                if(previous==current) {
                    if(!writer)break;
                    announced=true;current=desired;
                } else current=previous;
            } else if(!s.io.read(0,&current,sizeof(current))) {failed=true;throw TreeFault("structure latch read failed");}
            if(Clock::now()>=deadline){poison(s);throw TreeFault("structure latch deadline; cohort stopped");}
            std::this_thread::yield();
        }
        s.wait_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
        OffsetBTreeHeader h;
        if(!s.io.read(0,&h,sizeof(h)) || !valid_header(h,s.io.connection.remote_memory().length) || h.poison ||
           h.order!=initial.order || h.page_capacity!=initial.page_capacity ||
           h.record_capacity!=initial.record_capacity || h.seed_fingerprint!=initial.seed_fingerprint) {
            poison(s);throw TreeFault("shared data metadata/poison failure");
        }
        s.core->h=h;
    }
    void leave(Session& s,bool writer) const {
        std::uint64_t previous=0;
        if(writer) {
            if(!s.io.cas(writer_bit,0,previous) || previous!=writer_bit){poison(s);throw TreeFault("writer latch release failed");}
            return;
        }
        std::uint64_t current=0;
        const auto deadline=Clock::now()+std::chrono::milliseconds(timeout_ms);
        if(!s.io.read(0,&current,sizeof(current))) {failed=true;throw TreeFault("reader latch release read failed");}
        for(;;) {
            if(!(current&~writer_bit)){poison(s);throw TreeFault("reader latch corrupt");}
            if(!s.io.cas(current,current-1,previous)){failed=true;throw TreeFault("reader latch release failed");}
            if(previous==current)return;
            current=previous;
            if(Clock::now()>=deadline){poison(s);throw TreeFault("reader latch release deadline");}
        }
    }
};
BTreeRdma::BTreeRdma():impl_(new Impl){}
BTreeRdma::~BTreeRdma(){finish_thread();}
bool BTreeRdma::connect(const std::string& host,std::uint16_t port,std::size_t workers,
                       std::uint64_t expected_seed,std::uint64_t timeout_ms) {
    if(!workers || !timeout_ms || timeout_ms>600000 || !impl_->workers.empty())return false;
    impl_->timeout_ms=timeout_ms;
    auto open=[&](Session& s,bool first) {
        if(!s.io.connection.connect_client(host,port))return false;
        OffsetBTreeHeader h;
        if(!s.io.read(0,&h,sizeof(h)) || !valid_header(h,s.io.connection.remote_memory().length) || h.poison ||
           h.seed_fingerprint!=expected_seed)return false;
        if(first)impl_->initial=h;
        else if(h.order!=impl_->initial.order || h.bytes!=impl_->initial.bytes ||
                s.io.connection.remote_memory().address!=impl_->admin.io.connection.remote_memory().address)return false;
        s.core.reset(new TreeCore(s.io,h));return true;
    };
    if(!open(impl_->admin,true)){impl_->failed=true;return false;}
    for(std::size_t n=0;n<workers;++n) {
        auto s=std::make_unique<Session>();
        if(!open(*s,false)){impl_->failed=true;return false;}
        impl_->workers.push_back(std::move(s));
    }
    return true;
}
RC BTreeRdma::init(std::size_t order){return !impl_->failed && !impl_->workers.empty() && order==impl_->initial.order?RCOK:ERROR;}
void BTreeRdma::prepare_thread(std::size_t worker,std::size_t capacity) {
    if(attached_tree || worker>=impl_->workers.size())throw TreeFault("invalid worker attachment");
    auto& s=*impl_->workers[worker];
    if(s.attached)throw TreeFault("worker already attached");
    s.values.resize(std::min<std::size_t>(capacity,impl_->initial.record_capacity));
    s.attached=true;attached_tree=impl_.get();attached_session=&s;
}
void BTreeRdma::finish_thread() noexcept {
    if(attached_tree==impl_.get()) {
        attached_session->attached=false;attached_session=nullptr;attached_tree=nullptr;
    }
}
bool BTreeRdma::healthy() const{return !impl_->failed;}
bool BTreeRdma::lookup(idx_key_t key,void*& value) const {
    auto& s=impl_->session();
    try {
        impl_->enter(s,false);const bool found=s.core->lookup(key,s.point_value);impl_->leave(s,false);
        value=found?&s.point_value:nullptr;return found;
    } catch(...) {impl_->poison(s);throw;}
}
void BTreeRdma::scan_range_into(const KeyRangeEnt& range,std::vector<KeyValue>& out) const {
    auto& s=impl_->session();
    try {impl_->enter(s,false);s.core->scan(range,out,s.values);impl_->leave(s,false);}
    catch(...) {impl_->poison(s);throw;}
}
std::vector<KeyValue> BTreeRdma::scan_range(const KeyRangeEnt& range) const {
    auto& s=impl_->session();std::vector<KeyValue> out;out.reserve(s.values.size());scan_range_into(range,out);return out;
}
RC BTreeRdma::apply_batch(const std::vector<IndexMutation>& changes) {
    if(changes.empty())return RCOK;
    auto& s=impl_->session();
    try {
        impl_->enter(s,true);
        if(!s.core->capacity(changes)){impl_->leave(s,true);return AbortCapacity;}
        for(const auto& c:changes)s.core->mutate(c);
        s.core->publish();impl_->leave(s,true);return RCOK;
    } catch(...) {impl_->poison(s);throw;}
}
RC BTreeRdma::insert(idx_key_t key,void* value) {
    // Convenience API outside the common transaction driver.
    std::vector<IndexMutation> changes{{key,static_cast<const std::uint64_t*>(value),false}};
    return apply_batch(changes);
}
RC BTreeRdma::erase(idx_key_t key) {
    std::vector<IndexMutation> changes{{key,nullptr,true}};return apply_batch(changes);
}
std::size_t BTreeRdma::size() const {
    auto& s=impl_->session();
    try {impl_->enter(s,false);auto n=s.core->h.live_records;impl_->leave(s,false);return n;}
    catch(...) {impl_->poison(s);throw;}
}
namespace {
struct Validation {
    TreeCore& tree;
    std::vector<bool> pages,records;
    std::uint64_t last_leaf{0},next_leaf{0},live{0},indexed{0},seen_pages{0};
    explicit Validation(TreeCore& t):tree(t),pages(t.h.page_capacity),records(t.h.record_capacity){}
    std::pair<idx_key_t,idx_key_t> visit(std::uint64_t off,std::size_t depth) {
        if(depth>=64)throw TreeFault("validation depth");
        auto& n=tree.path[depth];tree.read_node(off,n);
        const auto id=(off-sizeof(OffsetBTreeHeader))/tree.h.page_bytes;
        if(pages[id])throw TreeFault("duplicate/cyclic page");
        pages[id]=true;++seen_pages;
        if(!n.head.count && (off!=tree.h.root || !n.head.leaf))throw TreeFault("empty nonroot page");
        if(n.head.leaf) {
            if(depth+1!=tree.h.height || n.head.prev!=last_leaf || (last_leaf && next_leaf!=off))throw TreeFault("leaf depth/chain");
            last_leaf=off;next_leaf=n.head.next;
            for(std::uint64_t k=0;k<n.head.count;++k) {
                OffsetBTreeRecord r;tree.read_record(n.links[k],r);
                auto rid=(n.links[k]-tree.h.record_base)/sizeof(r);
                if(records[rid] || r.key!=n.keys[k])throw TreeFault("duplicate/mismatched record");
                records[rid]=true;++indexed;live+=r.valid;
            }
            return n.head.count?std::make_pair(n.keys[0],n.keys[n.head.count-1]):std::pair<idx_key_t,idx_key_t>{0,0};
        }
        auto bounds=visit(n.links[0],depth+1);
        for(std::uint64_t k=0;k<n.head.count;++k) {
            auto child=visit(n.links[k+1],depth+1);
            if(bounds.second>=child.first || n.keys[k]!=child.first)throw TreeFault("separator/order");
            bounds.second=child.second;
        }
        return bounds;
    }
};
}
bool BTreeRdma::validate() const {
    auto& s=impl_->admin;
    try {
        Validation v(*s.core); // Allocation before acquiring any latch.
        impl_->enter(s,false);v.visit(s.core->h.root,0);
        const bool valid=!v.next_leaf && v.live==s.core->h.live_records &&
            v.indexed==s.core->h.records_used && v.seen_pages==s.core->h.pages_used;
        impl_->leave(s,false);if(!valid)impl_->poison(s);return valid;
    } catch(...) {impl_->poison(s);return false;}
}
std::uint64_t BTreeRdma::rdma_operations() const {
    std::uint64_t n=impl_->admin.io.connection.operation_count();for(const auto& s:impl_->workers)n+=s->io.connection.operation_count();return n;
}
std::uint64_t BTreeRdma::read_bytes() const {
    std::uint64_t n=impl_->admin.io.reads;for(const auto& s:impl_->workers)n+=s->io.reads;return n;
}
std::uint64_t BTreeRdma::write_bytes() const {
    std::uint64_t n=impl_->admin.io.writes;for(const auto& s:impl_->workers)n+=s->io.writes;return n;
}
std::uint64_t BTreeRdma::latch_wait_ns() const {
    std::uint64_t n=impl_->admin.wait_ns;for(const auto& s:impl_->workers)n+=s->wait_ns;return n;
}
std::size_t BTreeRdma::record_capacity() const{return impl_->initial.record_capacity;}
} // namespace rrl
