#pragma once
#include "index/btree.h"
#include <stdexcept>
namespace rrl::detail {
// Metadata snapshots use only short index latches. Transaction code locks
// stable identities and validates ancestry before consuming data.
inline bool host_leaf_cover(BTreePage* root,const KeyRangeEnt& range,
                            std::vector<LeafSpan>& out,bool concurrent) {
    out.clear();if(range.empty() || !root)return true;
    auto* page=root;idx_key_t lower=0;
    std::shared_lock<std::shared_mutex> held;
    if(concurrent)held=std::shared_lock<std::shared_mutex>(page->latch);
    while(!page->is_leaf) {
        const auto key=range.has_start?range.StartKey:0;
        const auto slot=std::upper_bound(page->keys.begin(),page->keys.end(),key)-page->keys.begin();
        if(slot)lower=page->keys[slot-1];
        auto* child=page->children[slot].get();
        std::shared_lock<std::shared_mutex> next;
        if(concurrent)next=std::shared_lock<std::shared_mutex>(child->latch);
        held=std::move(next);page=child;
    }
    for(;;) {
        auto* next=page->next;
        std::shared_lock<std::shared_mutex> next_guard;
        if(concurrent && next)next_guard=std::shared_lock<std::shared_mutex>(next->latch);
        auto fence=KeyRangeEnt::from_left(lower);
        if(next) {
            if(next->keys.empty())return false;
            fence.has_end=true;fence.EndKey=next->keys.front();fence.end_bound=Bound::Open;
        }
        if(fence.overlaps(range)) {
            if(out.size()==out.capacity())throw std::runtime_error("leaf cover workspace capacity");
            out.push_back({page->identity,fence});
        }
        if(!next || (range.has_end && (range.EndKey<fence.EndKey ||
            (range.EndKey==fence.EndKey && range.end_bound==Bound::Open))))return true;
        lower=fence.EndKey;page=next;held=std::move(next_guard);
    }
}
inline bool host_hierarchy_cover(BTreePage* root,const KeyRangeEnt& query,
                                 std::vector<IndexNodeSpan>& out,std::size_t order,bool concurrent) {
    out.clear();if(!root || query.empty())return true;
    auto visit=[&](auto&& self,BTreePage* page,std::size_t depth,const KeyRangeEnt& fence)->void {
        if(depth>=64)throw std::runtime_error("hierarchy depth");
        std::shared_lock<std::shared_mutex> latch;
        if(concurrent)latch=std::shared_lock<std::shared_mutex>(page->latch);
        if(out.size()==out.capacity())throw std::runtime_error("hierarchy workspace capacity");
        const bool present=page->is_leaf && query.has_start && std::binary_search(page->keys.begin(),page->keys.end(),query.StartKey);
        out.push_back({page->identity,fence,depth,page->keys.size(),page->children.size(),order-1,page->is_leaf,present});
        if(page->is_leaf)return;
        for(std::size_t i=0;i<page->children.size();++i) {
            auto range=fence;
            if(i){range.has_start=true;range.StartKey=page->keys[i-1];range.start_bound=Bound::Closed;}
            if(i<page->keys.size()){range.has_end=true;range.EndKey=page->keys[i];range.end_bound=Bound::Open;}
            if(range.overlaps(query))self(self,page->children[i].get(),depth+1,range);
        }
    };
    visit(visit,root,0,KeyRangeEnt::from_left(0));return true;
}

// Retain each parent while choosing/following its child; no upward latch
// acquisition or simultaneous right->left sibling latches. At most O(height)
// nodes for each extreme, including a cross-leaf predecessor/successor.
inline bool host_key_neighbors(BTreePage* root,idx_key_t key,IndexKeyNeighbors& out,bool concurrent) {
    out={};if(!root)return true;
    auto extreme=[&](auto&& self,BTreePage* page,bool largest)->idx_key_t {
        std::shared_lock<std::shared_mutex> held;
        if(concurrent)held=std::shared_lock<std::shared_mutex>(page->latch);
        if(page->is_leaf) {
            if(page->keys.empty())throw std::runtime_error("empty nonroot neighbor page");
            return largest?page->keys.back():page->keys.front();
        }
        return self(self,(largest?page->children.back():page->children.front()).get(),largest);
    };
    auto search=[&](auto&& self,BTreePage* page)->void {
        std::shared_lock<std::shared_mutex> held;
        if(concurrent)held=std::shared_lock<std::shared_mutex>(page->latch);
        if(page->is_leaf) {
            auto it=std::lower_bound(page->keys.begin(),page->keys.end(),key);
            out.present=it!=page->keys.end() && *it==key;
            if(it!=page->keys.begin()){out.has_predecessor=true;out.predecessor=*(it-1);}
            if(out.present)++it;
            if(it!=page->keys.end()){out.has_successor=true;out.successor=*it;}
            return;
        }
        const auto slot=static_cast<std::size_t>(std::upper_bound(page->keys.begin(),page->keys.end(),key)-page->keys.begin());
        self(self,page->children[slot].get());
        if(!out.has_predecessor && slot){out.predecessor=extreme(extreme,page->children[slot-1].get(),true);out.has_predecessor=true;}
        if(!out.has_successor && slot+1<page->children.size()){out.successor=extreme(extreme,page->children[slot+1].get(),false);out.has_successor=true;}
    };
    search(search,root);return true;
}
inline bool host_key_range(BTreePage* root,const KeyRangeEnt& range,std::vector<idx_key_t>& out,bool concurrent) {
    out.clear();if(!root || range.empty())return true;
    auto* page=root;std::shared_lock<std::shared_mutex> held;
    if(concurrent)held=std::shared_lock<std::shared_mutex>(page->latch);
    while(!page->is_leaf) {
        const auto slot=range.has_start?std::upper_bound(page->keys.begin(),page->keys.end(),range.StartKey)-page->keys.begin():0;
        auto* child=page->children[slot].get();std::shared_lock<std::shared_mutex> next;
        if(concurrent)next=std::shared_lock<std::shared_mutex>(child->latch);
        held=std::move(next);page=child;
    }
    for(;;) {
        for(auto key:page->keys) {
            if(range.has_end && (key>range.EndKey || (key==range.EndKey && range.end_bound==Bound::Open)))return true;
            if(range.contains(key))out.push_back(key);
        }
        auto* next=page->next;if(!next)return true;
        std::shared_lock<std::shared_mutex> guard;
        if(concurrent)guard=std::shared_lock<std::shared_mutex>(next->latch);
        held=std::move(guard);page=next;
    }
}

}
