#pragma once
#include "index/btree.h"
#include <stdexcept>
namespace rrl::detail {
// Topology is pinned by the transaction lock layer. Latches still make each
// physical read safe and keep this API independent of transaction locks.
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
}
