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

}
