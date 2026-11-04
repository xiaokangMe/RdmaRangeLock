#pragma once
#include "index/rdma_btree.h"
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>

namespace rrl::detail {
constexpr std::uint64_t tree_magic = 0x52524c4254523031ULL;
constexpr std::uint64_t writer_bit = std::uint64_t{1} << 63;
// No transactions or networking here: one offset page algorithm for a server
// image, deterministic tests, and every one-sided client protocol.
struct TreeIO {
    virtual ~TreeIO() = default;
    virtual bool read(std::uint64_t, void*, std::size_t) = 0;
    virtual bool write(std::uint64_t, const void*, std::size_t) = 0;
    virtual void record_enter(std::uint64_t, bool) {}
    virtual void record_leave(std::uint64_t, bool) {}
};
struct TreeFault : std::runtime_error { explicit TreeFault(const char* s):std::runtime_error(s){} };
struct TreeCapacity : std::runtime_error { TreeCapacity():std::runtime_error("shared tree capacity"){} };
inline std::uint64_t page_bytes(std::uint64_t order) {
    if(order<3 || order>4096)throw TreeFault("tree order must be 3..4096");
    return sizeof(OffsetBTreePage)+(2*order+1)*sizeof(std::uint64_t);
}
inline OffsetBTreeHeader make_header(const RdmaTreeConfig& c) {
    OffsetBTreeHeader h;h.magic=tree_magic;h.order=c.order;h.page_bytes=page_bytes(c.order);
    if(!c.page_capacity || !c.record_capacity)throw TreeFault("zero tree capacity");
    h.page_capacity=c.page_capacity;h.record_capacity=c.record_capacity;
    const auto max=std::numeric_limits<std::size_t>::max();
    if(h.page_capacity>(max-sizeof(h))/h.page_bytes)throw TreeFault("page capacity overflow");
    h.record_base=sizeof(h)+h.page_capacity*h.page_bytes;
    if(h.record_capacity>(max-h.record_base)/sizeof(OffsetBTreeRecord))throw TreeFault("record capacity overflow");
    h.bytes=h.record_base+h.record_capacity*sizeof(OffsetBTreeRecord);
    h.root=sizeof(h);h.pages_used=1;return h;
}
inline bool valid_header(const OffsetBTreeHeader& h,std::uint64_t length) {
    if(h.magic!=tree_magic || h.format!=2 || h.order<3 || h.order>4096 ||
       h.page_bytes!=page_bytes(h.order) || !h.page_capacity || !h.record_capacity)return false;
    try {
        const auto expected=make_header(RdmaTreeConfig{static_cast<std::size_t>(h.order),
            static_cast<std::size_t>(h.page_capacity),static_cast<std::size_t>(h.record_capacity),10000});
        return h.bytes==expected.bytes && h.bytes==length && h.record_base==expected.record_base &&
            h.pages_used && h.pages_used<=h.page_capacity && h.records_used<=h.record_capacity &&
            h.live_records<=h.records_used && h.height && h.height<=64 &&
            h.root>=sizeof(h) && (h.root-sizeof(h))%h.page_bytes==0 &&
            (h.root-sizeof(h))/h.page_bytes<h.pages_used;
    } catch(...) {return false;}
}
struct TreeNode {
    OffsetBTreePage head;
    std::vector<std::uint64_t> keys,links;
    explicit TreeNode(std::size_t order):keys(order+1),links(order+2){}
};
class TreeCore {
public:
    TreeIO& io;
    OffsetBTreeHeader h;
    // All buffers allocated before transactions. Path has one slot per possible
    // uint64-key tree level, plus two split workspaces.
    std::vector<TreeNode> path;
    std::vector<std::uint64_t> offsets,slots;
    std::vector<std::uint8_t> wire;
    explicit TreeCore(TreeIO& i,const OffsetBTreeHeader& header):io(i),h(header),offsets(64),slots(64),wire(h.page_bytes) {
        path.reserve(66);for(std::size_t n=0;n<66;++n)path.emplace_back(h.order);
    }
    void get(std::uint64_t off,void* p,std::size_t n) {
        if(off>h.bytes || n>h.bytes-off || !io.read(off,p,n))throw TreeFault("tree read failed/bounds");
    }
    void put(std::uint64_t off,const void* p,std::size_t n) {
        if(off>h.bytes || n>h.bytes-off || !io.write(off,p,n))throw TreeFault("tree write failed/bounds");
    }
    void read_node(std::uint64_t off,TreeNode& n) {
        if(off<sizeof(h) || (off-sizeof(h))%h.page_bytes ||
           (off-sizeof(h))/h.page_bytes>=h.pages_used)throw TreeFault("invalid page offset");
        get(off,wire.data(),wire.size());std::memcpy(&n.head,wire.data(),sizeof(n.head));
        if(n.head.leaf>1 || n.head.count>=h.order)throw TreeFault("invalid page header");
        std::memcpy(n.keys.data(),wire.data()+sizeof(n.head),h.order*8);
        std::memcpy(n.links.data(),wire.data()+sizeof(n.head)+h.order*8,(h.order+1)*8);
        if(!std::is_sorted(n.keys.begin(),n.keys.begin()+n.head.count) ||
           std::adjacent_find(n.keys.begin(),n.keys.begin()+n.head.count)!=n.keys.begin()+n.head.count)
            throw TreeFault("unsorted page");
    }
    void write_node(std::uint64_t off,const TreeNode& n) {
        if(n.head.count>=h.order)throw TreeFault("page overflow");
        std::memcpy(wire.data(),&n.head,sizeof(n.head));
        std::memcpy(wire.data()+sizeof(n.head),n.keys.data(),h.order*8);
        std::memcpy(wire.data()+sizeof(n.head)+h.order*8,n.links.data(),(h.order+1)*8);
        put(off,wire.data(),wire.size());
    }
    void read_record(std::uint64_t off,OffsetBTreeRecord& r) {
        if(off<h.record_base || (off-h.record_base)%sizeof(r) ||
           (off-h.record_base)/sizeof(r)>=h.records_used)throw TreeFault("invalid record offset");
        io.record_enter(off,false);
        get(off,&r,sizeof(r));
        io.record_leave(off,false);
        r.latch=0; // Never publish a snapshot of the reader count.
        if(r.valid>1)throw TreeFault("invalid record flag");
    }
    std::size_t descend(idx_key_t key) {
        auto off=h.root;
        for(std::size_t depth=0;depth<64;++depth) {
            auto& n=path[depth];read_node(off,n);offsets[depth]=off;
            if(n.head.leaf) {
                slots[depth]=std::lower_bound(n.keys.begin(),n.keys.begin()+n.head.count,key)-n.keys.begin();
                return depth;
            }
            auto slot=std::upper_bound(n.keys.begin(),n.keys.begin()+n.head.count,key)-n.keys.begin();
            slots[depth]=slot;off=n.links[slot];
        }
        throw TreeFault("tree depth/cycle");
    }
    bool lookup(idx_key_t key,std::uint64_t& value) {
        auto d=descend(key);auto& n=path[d];auto slot=slots[d];
        if(slot==n.head.count || n.keys[slot]!=key)return false;
        OffsetBTreeRecord r;read_record(n.links[slot],r);
        if(r.key!=key)throw TreeFault("record key mismatch");
        value=r.value;return r.valid;
    }
    std::uint64_t new_page() {
        if(h.pages_used==h.page_capacity)throw TreeCapacity();
        return sizeof(h)+h.pages_used++*h.page_bytes;
    }
    // Conservative preflight for the entire batch, before its first WRITE.
    bool capacity(const std::vector<IndexMutation>& changes) {
        std::uint64_t fresh=0;
        for(const auto& c:changes)if(!c.remove) {
            if(!c.value)throw TreeFault("null mutation value");
            auto d=descend(c.key);const auto& n=path[d];
            if(slots[d]==n.head.count || n.keys[slots[d]]!=c.key)++fresh;
        }
        // Every insertion adds at most one tree level. Reserve a conservative
        // whole-batch bound before any mutation; repeated keys overestimate.
        return fresh<=h.record_capacity-h.records_used &&
            fresh<=(h.page_capacity-h.pages_used)/std::min<std::uint64_t>(65,h.height+fresh+1);
    }
    void mutate(const IndexMutation& c) {
        auto depth=descend(c.key);auto& leaf=path[depth];auto slot=slots[depth];
        if(slot<leaf.head.count && leaf.keys[slot]==c.key) {
            OffsetBTreeRecord r;read_record(leaf.links[slot],r);
            if(r.key!=c.key)throw TreeFault("record key mismatch");
            const auto valid=c.remove?0ULL:1ULL;
            if(valid!=r.valid) {if(valid)++h.live_records;else --h.live_records;}
            r.valid=valid;if(!c.remove)r.value=*c.value;
            put(leaf.links[slot],&r,sizeof(r));return;
        }
        if(c.remove)return;
        if(h.records_used==h.record_capacity)throw TreeCapacity();
        OffsetBTreeRecord r{c.key,*c.value,1};
        const auto roff=h.record_base+h.records_used++*sizeof(r);put(roff,&r,sizeof(r));++h.live_records;
        std::move_backward(leaf.keys.begin()+slot,leaf.keys.begin()+leaf.head.count,leaf.keys.begin()+leaf.head.count+1);
        std::move_backward(leaf.links.begin()+slot,leaf.links.begin()+leaf.head.count,leaf.links.begin()+leaf.head.count+1);
        leaf.keys[slot]=c.key;leaf.links[slot]=roff;++leaf.head.count;
        for(;;) {
            auto& n=path[depth];const auto off=offsets[depth];
            if(n.head.count<h.order){write_node(off,n);return;}
            auto& right=path[64];right.head=OffsetBTreePage{};right.head.leaf=n.head.leaf;
            const auto right_off=new_page();
            const auto middle=n.head.count/2;
            const auto separator=n.keys[middle];
            if(n.head.leaf) {
                right.head.count=n.head.count-middle;
                std::copy_n(n.keys.begin()+middle,right.head.count,right.keys.begin());
                std::copy_n(n.links.begin()+middle,right.head.count,right.links.begin());
                right.head.prev=off;right.head.next=n.head.next;
                if(n.head.next) {auto& next=path[65];read_node(n.head.next,next);next.head.prev=right_off;write_node(n.head.next,next);}
                n.head.next=right_off;
            } else {
                right.head.count=n.head.count-middle-1;
                std::copy_n(n.keys.begin()+middle+1,right.head.count,right.keys.begin());
                std::copy_n(n.links.begin()+middle+1,right.head.count+1,right.links.begin());
            }
            n.head.count=middle;write_node(off,n);write_node(right_off,right);
            if(!depth) {
                auto& root=path[65];root.head=OffsetBTreePage{};root.head.leaf=0;root.head.count=1;
                root.keys[0]=separator;root.links[0]=off;root.links[1]=right_off;
                h.root=new_page();++h.height;write_node(h.root,root);return;
            }
            --depth;auto& parent=path[depth];auto pos=slots[depth];
            std::move_backward(parent.keys.begin()+pos,parent.keys.begin()+parent.head.count,parent.keys.begin()+parent.head.count+1);
            std::move_backward(parent.links.begin()+pos+1,parent.links.begin()+parent.head.count+1,parent.links.begin()+parent.head.count+2);
            parent.keys[pos]=separator;parent.links[pos+1]=right_off;++parent.head.count;
        }
    }
    // Do not overwrite the NIC-owned latch/poison words with a stale header.
    void publish() {put(16,reinterpret_cast<const char*>(&h)+16,sizeof(h)-16);}
    void scan(const KeyRangeEnt& range,std::vector<KeyValue>& out,std::vector<std::uint64_t>& values) {
        out.clear();if(range.empty())return;
        auto d=descend(range.has_start?range.StartKey:0);auto off=offsets[d];
        auto& n=path[0];std::uint64_t visited=0;
        while(off) {
            if(++visited>h.pages_used)throw TreeFault("leaf cycle");
            read_node(off,n);if(!n.head.leaf)throw TreeFault("nonleaf in chain");
            for(std::uint64_t i=0;i<n.head.count;++i) {
                const auto key=n.keys[i];
                if(range.has_end && (key>range.EndKey || (key==range.EndKey && range.end_bound==Bound::Open)))return;
                if(!range.contains(key))continue;
                OffsetBTreeRecord r;read_record(n.links[i],r);
                if(r.key!=key)throw TreeFault("scan record key mismatch");
                if(!r.valid)continue;
                if(out.size()==out.capacity() || out.size()==values.size())throw TreeFault("scan workspace capacity");
                values[out.size()]=r.value;out.push_back({key,&values[out.size()]});
            }
            off=n.head.next;
        }
    }
};
} // namespace rrl::detail
