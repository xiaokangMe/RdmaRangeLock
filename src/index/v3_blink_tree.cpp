#include "leaf_cover.h"
#include "index/btree.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

namespace rrl {
namespace {
constexpr std::size_t kMinOrder=3;
}
RC BTreeBlink::init(std::size_t order) {
    if(order<kMinOrder)return ERROR;
    std::unique_lock<std::shared_mutex> guard(root_guard_);
    order_=order;root_.reset();size_.store(0,std::memory_order_relaxed);
    return RCOK;
}
std::unique_ptr<BTreeBlink::Page> BTreeBlink::make_page(bool leaf) const {
    auto page=std::make_unique<Page>();
    page->identity=next_page_identity_.fetch_add(1);
    page->is_leaf=leaf;page->keys.reserve(order_);
    if(leaf)page->values.reserve(order_);else page->children.reserve(order_+1);
    return page;
}
std::size_t BTreeBlink::page_slot(const Page& parent,const Page* child) {
    for(std::size_t i=0;i<parent.children.size();++i)if(parent.children[i].get()==child)return i;
    return parent.children.size();
}
idx_key_t BTreeBlink::first_key(const Page& page) {
    const Page* current=&page;
    while(!current->is_leaf)current=current->children.front().get();
    return current->keys.empty()?0:current->keys.front();
}
bool BTreeBlink::range_ended(const KeyRangeEnt& range,idx_key_t key) {
    return range.has_end && (key>range.EndKey || (key==range.EndKey && range.end_bound==Bound::Open));
}
BTreeBlink::Page* BTreeBlink::find_leaf_blink(idx_key_t key) const {
    Page* page=root_.get();if(!page)return nullptr;
    std::shared_lock<std::shared_mutex> latch(page->latch);
    for(std::size_t depth=0;depth<128;++depth) {
        while(page->has_high_key && key>=page->high_key) {
            Page* right=page->right_link;
            if(!right)return nullptr;
            std::shared_lock<std::shared_mutex> next(right->latch);
            latch=std::move(next);page=right;
        }
        if(page->is_leaf)return page;
        const auto pos=std::upper_bound(page->keys.begin(),page->keys.end(),key);
        Page* child=page->children[static_cast<std::size_t>(pos-page->keys.begin())].get();
        std::shared_lock<std::shared_mutex> next(child->latch);
        latch=std::move(next);page=child;
    }
    throw std::runtime_error("B-link descent depth");
}
RC BTreeBlink::insert_fast(idx_key_t key,void* value) {
    for(unsigned attempt=0;attempt<16;++attempt) {
        std::shared_lock<std::shared_mutex> root_guard(root_guard_);
        if(!root_)return WAIT;
        Page* leaf=find_leaf_blink(key);if(!leaf)return WAIT;
        std::unique_lock<std::shared_mutex> leaf_guard(leaf->latch);
        if(leaf->has_high_key && key>=leaf->high_key)continue;
        const auto pos=std::lower_bound(leaf->keys.begin(),leaf->keys.end(),key);
        const auto index=static_cast<std::size_t>(pos-leaf->keys.begin());
        if(pos!=leaf->keys.end() && *pos==key){leaf->values[index]=value;return RCOK;}
        if(leaf->keys.size()+1<order_) {
            leaf->keys.insert(pos,key);leaf->values.insert(leaf->values.begin()+static_cast<std::ptrdiff_t>(index),value);
            size_.fetch_add(1,std::memory_order_relaxed);return RCOK;
        }
        return WAIT;
    }
    return WAIT;
}
std::unique_ptr<BTreeBlink::Page> BTreeBlink::insert_recursive(Page& page,idx_key_t key,void* value,idx_key_t& separator) {
    if(page.is_leaf) {
        const auto pos=std::lower_bound(page.keys.begin(),page.keys.end(),key);
        const auto index=static_cast<std::size_t>(pos-page.keys.begin());
        if(pos!=page.keys.end() && *pos==key){page.values[index]=value;return nullptr;}
        page.keys.insert(pos,key);page.values.insert(page.values.begin()+static_cast<std::ptrdiff_t>(index),value);
        size_.fetch_add(1,std::memory_order_relaxed);
        if(page.keys.size()<order_)return nullptr;
        auto right=make_page(true);const auto middle=page.keys.size()/2;
        right->keys.assign(page.keys.begin()+static_cast<std::ptrdiff_t>(middle),page.keys.end());
        right->values.assign(page.values.begin()+static_cast<std::ptrdiff_t>(middle),page.values.end());
        page.keys.resize(middle);page.values.resize(middle);separator=right->keys.front();
        right->prev=&page;right->next=page.next;
        if(page.next) page.next->prev=right.get();
        page.next=right.get();
        right->parent=page.parent;return right;
    }
    const auto pos=std::upper_bound(page.keys.begin(),page.keys.end(),key);
    const auto child_index=static_cast<std::size_t>(pos-page.keys.begin());
    idx_key_t child_separator=0;
    auto right_child=insert_recursive(*page.children[child_index],key,value,child_separator);
    if(!right_child)return nullptr;
    page.keys.insert(page.keys.begin()+static_cast<std::ptrdiff_t>(child_index),child_separator);
    page.children.insert(page.children.begin()+static_cast<std::ptrdiff_t>(child_index+1),std::move(right_child));
    page.children[child_index+1]->parent=&page;
    if(page.keys.size()<order_)return nullptr;
    auto right=make_page(false);const auto middle=page.keys.size()/2;separator=page.keys[middle];
    right->keys.assign(page.keys.begin()+static_cast<std::ptrdiff_t>(middle+1),page.keys.end());
    for(std::size_t i=middle+1;i<page.children.size();++i) {
        right->children.push_back(std::move(page.children[i]));right->children.back()->parent=right.get();
    }
    page.keys.resize(middle);page.children.resize(middle+1);right->parent=page.parent;return right;
}
void BTreeBlink::rebuild_links() {
    if(!root_)return;
    std::vector<std::vector<Page*>> levels;
    std::vector<std::vector<idx_key_t>> mins;
    std::function<idx_key_t(Page*,std::size_t)> visit=[&](Page* page,std::size_t level)->idx_key_t {
        if(levels.size()<=level){levels.emplace_back();mins.emplace_back();}
        levels[level].push_back(page);
        idx_key_t minimum=0;
        if(page->is_leaf)minimum=page->keys.empty()?0:page->keys.front();
        else {
            for(auto& child:page->children)visit(child.get(),level+1);
            minimum=page->children.empty()?0:first_key(*page->children.front());
        }
        mins[level].push_back(minimum);return minimum;
    };
    visit(root_.get(),0);
    for(std::size_t level=0;level<levels.size();++level) {
        auto& pages=levels[level];auto& keys=mins[level];
        for(std::size_t i=0;i<pages.size();++i) {
            auto* page=pages[i];
            page->right_link=i+1<pages.size()?pages[i+1]:nullptr;
            page->has_high_key=i+1<pages.size();
            if(page->has_high_key)page->high_key=keys[i+1];
        }
    }
    // The leaf level is the deepest level; set its doubly linked chain.
    auto& leaves=levels.back();
    for(std::size_t i=0;i<leaves.size();++i) {
        leaves[i]->prev=i?leaves[i-1]:nullptr;
        leaves[i]->next=i+1<leaves.size()?leaves[i+1]:nullptr;
        leaves[i]->right_link=leaves[i]->next;
        leaves[i]->has_high_key=i+1<leaves.size();
        if(leaves[i]->has_high_key)leaves[i]->high_key=mins.back()[i+1];
    }
    root_->right_link=nullptr;root_->has_high_key=false;
}
RC BTreeBlink::insert_exclusive(idx_key_t key,void* value) {
    std::unique_lock<std::shared_mutex> guard(root_guard_);
    if(!root_) {
        root_=make_page(true);root_->keys.push_back(key);root_->values.push_back(value);
        size_.store(1,std::memory_order_relaxed);rebuild_links();return RCOK;
    }
    idx_key_t separator=0;auto right=insert_recursive(*root_,key,value,separator);
    if(right) {
        auto new_root=make_page(false);
        new_root->keys.push_back(separator);new_root->children.push_back(std::move(root_));new_root->children.push_back(std::move(right));
        new_root->children[0]->parent=new_root.get();new_root->children[1]->parent=new_root.get();root_=std::move(new_root);
    }
    rebuild_links();return RCOK;
}
RC BTreeBlink::insert(idx_key_t key,void* value) {
    if(order_==0)return ERROR;
    for(unsigned attempt=0;attempt<8;++attempt) {
        const auto rc=insert_fast(key,value);
        if(rc==RCOK)return RCOK;
    }
    return insert_exclusive(key,value);
}
RC BTreeBlink::erase(idx_key_t key) {
    if(order_==0)return Abort;
    std::unique_lock<std::shared_mutex> guard(root_guard_);
    if(!root_)return Abort;
    std::vector<Page*> path;Page* page=root_.get();path.push_back(page);
    while(!page->is_leaf) {
        const auto pos=std::upper_bound(page->keys.begin(),page->keys.end(),key);
        page=page->children[static_cast<std::size_t>(pos-page->keys.begin())].get();path.push_back(page);
    }
    const auto pos=std::lower_bound(page->keys.begin(),page->keys.end(),key);
    if(pos==page->keys.end() || *pos!=key)return Abort;
    const auto index=static_cast<std::size_t>(pos-page->keys.begin());
    page->keys.erase(pos);page->values.erase(page->values.begin()+static_cast<std::ptrdiff_t>(index));
    size_.fetch_sub(1,std::memory_order_relaxed);
    std::vector<std::unique_ptr<Page>> retired;
    for(std::size_t depth=path.size();depth-- > 1;) {
        Page* child=path[depth];Page* parent=path[depth-1];const auto slot=page_slot(*parent,child);
        if(slot==parent->children.size())return ERROR;
        if(child->keys.empty() && (child->is_leaf || child->children.empty())) {
            if(child->is_leaf){if(child->prev)child->prev->next=child->next;if(child->next)child->next->prev=child->prev;}
            auto removed=std::move(parent->children[slot]);parent->children.erase(parent->children.begin()+static_cast<std::ptrdiff_t>(slot));
            if(slot>0)parent->keys.erase(parent->keys.begin()+static_cast<std::ptrdiff_t>(slot-1));
            else if(!parent->keys.empty())parent->keys.erase(parent->keys.begin());
            retired.push_back(std::move(removed));continue;
        }
    }
    while(root_ && !root_->is_leaf && root_->keys.empty() && root_->children.size()==1) {
        auto old=std::move(root_);root_=std::move(old->children.front());root_->parent=nullptr;retired.push_back(std::move(old));
    }
    if(root_ && !root_->is_leaf && root_->keys.empty() && root_->children.empty())root_.reset();
    rebuild_links();return RCOK;
}
bool BTreeBlink::lookup(idx_key_t key,void*& value) const {
    std::shared_lock<std::shared_mutex> guard(root_guard_);
    for(;;) {
        Page* page=find_leaf_blink(key);if(!page)return false;
        std::shared_lock<std::shared_mutex> latch(page->latch);
        if(page->has_high_key && key>=page->high_key)continue;
        const auto pos=std::lower_bound(page->keys.begin(),page->keys.end(),key);
        if(pos==page->keys.end() || *pos!=key)return false;
        value=page->values[static_cast<std::size_t>(pos-page->keys.begin())];return true;
    }
}
std::vector<KeyValue> BTreeBlink::scan_range(const KeyRangeEnt& range) const {
    std::vector<KeyValue> out;if(range.empty() || !range.valid())return out;
    std::shared_lock<std::shared_mutex> guard(root_guard_);
    Page* page=find_leaf_blink(range.has_start?range.StartKey:0);if(!page)return out;
    for(;;) {
        std::shared_lock<std::shared_mutex> latch(page->latch);
        if(page->has_high_key && range.has_start && range.StartKey>=page->high_key) {
            page=page->right_link;if(!page)break;continue;
        }
        for(std::size_t i=0;i<page->keys.size();++i) {
            const auto key=page->keys[i];if(range_ended(range,key))return out;
            if(range.contains(key))out.push_back({key,page->values[i]});
        }
        Page* next=page->next;if(!next)break;page=next;
        if(range.has_end && page->keys.empty())continue;
        if(range.has_end && page->keys.size() && range_ended(range,page->keys.front()))break;
    }
    return out;
}
std::size_t BTreeBlink::size() const {return size_.load(std::memory_order_relaxed);}
std::size_t BTreeBlink::height() const {
    std::shared_lock<std::shared_mutex> guard(root_guard_);std::size_t h=0;
    for(Page* page=root_.get();page;++h)page=page->is_leaf?nullptr:page->children.front().get();
    return h;
}
bool BTreeBlink::key_neighbors(idx_key_t key,IndexKeyNeighbors& out) const {
    std::shared_lock<std::shared_mutex> guard(root_guard_);return detail::host_key_neighbors(root_.get(),key,out,true);
}
bool BTreeBlink::key_range_into(const KeyRangeEnt& range,std::vector<idx_key_t>& out) const {
    std::shared_lock<std::shared_mutex> guard(root_guard_);return detail::host_key_range(root_.get(),range,out,true);
}
bool BTreeBlink::leaf_cover_into(const KeyRangeEnt& range,std::vector<LeafSpan>& out) const {
    std::shared_lock<std::shared_mutex> guard(root_guard_);return detail::host_leaf_cover(root_.get(),range,out,true);
}
bool BTreeBlink::hierarchy_cover_into(const KeyRangeEnt& range,std::vector<IndexNodeSpan>& out) const {
    std::shared_lock<std::shared_mutex> guard(root_guard_);return detail::host_hierarchy_cover(root_.get(),range,out,order_,true);
}
bool BTreeBlink::validate_page(const Page* page,const Page* parent,std::size_t depth,idx_key_t* minimum,
                               std::size_t* leaves,std::size_t* keys,std::size_t* leaf_depth) const {
    if(!page || page->parent!=parent || !std::is_sorted(page->keys.begin(),page->keys.end()))return false;
    if(page->is_leaf){
        if(*leaf_depth==std::numeric_limits<std::size_t>::max())*leaf_depth=depth;
        else if(*leaf_depth!=depth)return false;
        if(*leaves==0)*minimum=page->keys.empty()?0:page->keys.front();
        *keys+=page->keys.size();++*leaves;return true;
    }
    if(page->children.size()!=page->keys.size()+1 || page->children.empty())return false;
    idx_key_t prior=0;bool first=true;
    for(const auto& child:page->children) {
        idx_key_t child_min=0;std::size_t count=0,child_keys=0;
        if(!validate_page(child.get(),page,depth+1,&child_min,&count,&child_keys,leaf_depth))return false;
        if(!first && child_min<prior) return false;
        first=false;prior=child_min;*leaves+=count;*keys+=child_keys;
    }
    return true;
}
bool BTreeBlink::validate() const {
    std::shared_lock<std::shared_mutex> guard(root_guard_);if(!root_)return size()==0;
    idx_key_t min=0;std::size_t leaves=0,keys=0,leaf_depth=std::numeric_limits<std::size_t>::max();
    if(!validate_page(root_.get(),nullptr,0,&min,&leaves,&keys,&leaf_depth))return false;
    std::vector<Page*> pages;std::function<void(Page*,std::size_t)> collect=[&](Page* p,std::size_t){pages.push_back(p);if(!p->is_leaf)for(auto& c:p->children)collect(c.get(),0);};collect(root_.get(),0);
    for(auto* p:pages) {
        if(p->has_high_key && (!p->right_link || p->is_leaf!=p->right_link->is_leaf || first_key(*p->right_link)!=p->high_key))return false;
        if(!p->has_high_key && p->right_link)return false;
        if(p->is_leaf && p->right_link!=p->next)return false;
    }
    return leaves>0 && keys==size();
}
} // namespace rrl
