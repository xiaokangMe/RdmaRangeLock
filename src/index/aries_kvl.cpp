#include "index/aries_kvl.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace rrl::aries {
namespace {
using Clock=std::chrono::steady_clock;
bool less(const Entry& a,const Entry& b) {return std::tie(a.key,a.rid)<std::tie(b.key,b.rid);}
bool same_pair(const Entry& a,const Entry& b) {return a.key==b.key && a.rid==b.rid;}
struct Name {
    // Each Index owns a separate lock manager, providing the index-ID namespace.
    enum Kind { Key, Record, Eof } kind{Key};
    std::uint64_t key{0};
    bool operator<(const Name& other) const {return std::tie(kind,key)<std::tie(other.kind,other.key);}
    bool operator==(const Name& other) const {return kind==other.kind && key==other.key;}
};
Name key_name(idx_key_t key) {return {Name::Key,key};}
Name eof_name() {return {Name::Eof,0};}
struct Holding {LockMode mode;Duration duration;};
struct Reply {Status status;std::optional<Holding> previous;};

// Online lock names; no trace, endpoint partition, interval scan or key universe.
// Transactions wait only in this manager, never with page latches held.
struct Locks {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<Name,std::map<txnid_t,Holding>> table;
    std::map<txnid_t,std::set<Name>> owned;
    std::map<txnid_t,std::set<txnid_t>> waiting;
    std::size_t peak{0};
    std::atomic<std::uint64_t> requests{0},instants{0},waits{0};
    bool cycle(txnid_t node,txnid_t target,std::set<txnid_t>& seen) {
        if(!seen.insert(node).second)return false;
        const auto found=waiting.find(node);
        if(found==waiting.end())return false;
        for(auto next:found->second)if(next==target || cycle(next,target,seen))return true;
        return false;
    }
    Reply request(txnid_t txn,Name name,LockMode mode,Duration duration,bool conditional,Clock::time_point deadline) {
        ++requests;if(duration==Duration::Instant)++instants;
        std::unique_lock<std::mutex> guard(mutex);
        auto old=[&]()->std::optional<Holding> {
            const auto resource=table.find(name);if(resource==table.end())return {};
            const auto holder=resource->second.find(txn);if(holder==resource->second.end())return {};
            return holder->second;
        }();
        const auto result=old?lock_mode_join(old->mode,mode):mode;
        bool counted=false;
        for(;;) {
            std::set<txnid_t> blockers;
            const auto resource=table.find(name);
            if(resource!=table.end())for(const auto& h:resource->second)
                if(h.first!=txn && lock_modes_conflict(result,h.second.mode))blockers.insert(h.first);
            if(blockers.empty()) {
                waiting.erase(txn);
                // Instant means compatibility test only, not grant then release.
                if(duration!=Duration::Instant) {
                    const auto keep=old && old->duration==Duration::Commit?Duration::Commit:duration;
                    table[name][txn]={result,keep};owned[txn].insert(name);peak=std::max(peak,table.size());
                }
                return {Status::Ok,old};
            }
            if(conditional)return {Status::NotFound,old}; // internal "not immediately grantable"
            waiting[txn]=std::move(blockers);
            std::set<txnid_t> seen;
            if(cycle(txn,txn,seen)) {waiting.erase(txn);return {Status::Deadlock,old};}
            if(!counted){++waits;counted=true;}
            const auto remaining=deadline-Clock::now();
            if(remaining<=Clock::duration::zero()) {
                waiting.erase(txn);return {Status::Timeout,old};
            }
            // Use the portable realtime CV entry point (older TSan runtimes do
            // not intercept pthread_cond_clockwait). Steady time still decides
            // expiry; a realtime timeout or spurious wake rechecks the predicate.
            changed.wait_until(guard,std::chrono::system_clock::now()+remaining);
        }
    }
    void restore(txnid_t txn,Name name,std::optional<Holding> prior) {
        std::lock_guard<std::mutex> guard(mutex);
        auto resource=table.find(name);
        if(prior) {table[name][txn]=*prior;owned[txn].insert(name);}
        else if(resource!=table.end()) {
            resource->second.erase(txn);if(resource->second.empty())table.erase(resource);
            auto own=owned.find(txn);if(own!=owned.end()) {own->second.erase(name);if(own->second.empty())owned.erase(own);}
        }
        changed.notify_all();
    }
    void finish(txnid_t txn) {
        std::lock_guard<std::mutex> guard(mutex);
        const auto own=owned.find(txn);
        if(own!=owned.end()) {
            for(const auto& name:own->second) {
                auto resource=table.find(name);
                if(resource!=table.end()) {resource->second.erase(txn);if(resource->second.empty())table.erase(resource);}
            }
            owned.erase(own);
        }
        waiting.erase(txn);changed.notify_all();
    }
    std::optional<LockMode> held(txnid_t txn,Name name) {
        std::lock_guard<std::mutex> guard(mutex);
        const auto resource=table.find(name);if(resource==table.end())return {};
        const auto holder=resource->second.find(txn);if(holder==resource->second.end())return {};
        return holder->second.mode;
    }
};

struct Page {
    std::uint64_t id{0},version{1};
    bool leaf{true},sm{false},retired{false};
    std::vector<Entry> keys;
    std::vector<Page*> children;
    Page *parent{nullptr},*prev{nullptr},*next{nullptr};
    mutable std::shared_mutex latch;
};
struct Hold {
    Page* page{nullptr};unsigned* count{nullptr};
    std::shared_lock<std::shared_mutex> shared;
    std::unique_lock<std::shared_mutex> exclusive;
    Hold()=default;
    Hold(Page* p,bool write,unsigned& n,std::atomic<std::size_t>& maximum):page(p),count(&n) {
        if(write)exclusive=std::unique_lock<std::shared_mutex>(p->latch);
        else shared=std::shared_lock<std::shared_mutex>(p->latch);
        ++n;auto old=maximum.load();while(old<n && !maximum.compare_exchange_weak(old,n)){}
        if(n>2)throw std::logic_error("ARIES/KVL holds at most two page latches");
    }
    Hold(Hold&& other) noexcept { *this=std::move(other); }
    Hold& operator=(Hold&& other) noexcept {
        if(this!=&other) {
            release();page=other.page;count=other.count;shared=std::move(other.shared);exclusive=std::move(other.exclusive);
            other.page=nullptr;other.count=nullptr;
        }return *this;
    }
    void release() {
        if(!page)return;
        if(exclusive.owns_lock())exclusive.unlock();
        if(shared.owns_lock())shared.unlock();
        --*count;page=nullptr;count=nullptr;
    }
    ~Hold(){release();}
};
struct Undo {bool inserted;Entry entry;};
struct Temporary {Name name;std::optional<Holding> prior;};
}

struct Index::Transaction::State {
    Impl* index;
    txnid_t id;
    bool active{true};
    unsigned page_latches{0};
    std::vector<Undo> undo;
};

struct Index::Impl {
    std::size_t order;bool unique;
    std::chrono::milliseconds timeout;
    std::vector<std::unique_ptr<Page>> pages; // retired pages retained: cursor pointers remain valid
    std::mutex page_directory;
    Page* root;
    std::shared_mutex tree_latch; // only SMOs request X; ordinary actions never take S
    Locks locks;
    std::atomic<txnid_t> next_txn{1};
    std::atomic<std::size_t> records{0},max_latches{0};
    std::atomic<std::uint64_t> revalidations{0},replications{0},splits{0},page_deletes{0};
    std::atomic<std::uint64_t> inserts{0},deletes{0},undos{0},cursor_fast{0},cursor_repositions{0};
    std::function<void(const Event&)> observer;
    using State=Transaction::State;

    Impl(std::size_t o,bool u,std::chrono::milliseconds t):order(o),unique(u),timeout(t) {
        if(o<4 || t.count()<=0)throw std::invalid_argument("ARIES/KVL order >= 4 and positive lock timeout required");
        root=make_page(true);
    }
    Page* make_page(bool leaf) {
        std::lock_guard<std::mutex> directory(page_directory);
        auto page=std::make_unique<Page>();page->id=pages.size()+1;page->leaf=leaf;
        page->keys.reserve(order);page->children.reserve(order+1);
        auto* result=page.get();pages.push_back(std::move(page));return result;
    }
    void event(State& tx,Phase phase,Name name,Page* p=nullptr,LockMode mode=LockMode::S,Duration d=Duration::Commit) {
        if(observer)observer({phase,tx.id,name.key,name.kind==Name::Eof,p?p->id:0,p?p->version:0,tx.page_latches,mode,d});
    }
    bool valid(const Transaction& tx) const {return tx.state_ && tx.state_->index==this && tx.state_->active;}
    void posc(State& tx) {
        if(tx.page_latches)throw std::logic_error("wait for POSC while holding a page latch");
        std::shared_lock<std::shared_mutex> wait(tree_latch);
    }
    // Figure 5: parent is held until the child has its appropriate S/X latch.
    Hold search(State& tx,const Entry& probe,bool write) {
        for(;;) {
            Hold current(root,false,tx.page_latches,max_latches);
            if(root->leaf && write) {
                current.release();current=Hold(root,true,tx.page_latches,max_latches);
                if(!root->leaf)continue;
            }
            bool restart=false;
            for(;;) {
                Page* page=current.page;
                if(page->sm || page->retired) {current.release();posc(tx);restart=true;break;}
                if(page->leaf){event(tx,Phase::SearchLeaf,key_name(probe.key),page);return current;}
                const auto slot=std::upper_bound(page->keys.begin(),page->keys.end(),probe,less)-page->keys.begin();
                Page* child=page->children[slot];
                // All nonroot page types remain fixed for their entire lifetime.
                Hold next(child,write && child->leaf,tx.page_latches,max_latches);
                current=std::move(next);
            }
            if(restart)continue;
        }
    }
    Reply conditional(State& tx,Name name,LockMode mode,Duration duration,Page* page) {
        auto reply=locks.request(tx.id,name,mode,duration,true,Clock::now());
        event(tx,Phase::ConditionalLock,name,page,mode,duration);return reply;
    }
    Status wait(State& tx,Name name,LockMode mode,Duration duration,std::optional<Temporary>& temporary) {
        if(tx.page_latches)throw std::logic_error("transaction lock wait with page latch held");
        event(tx,Phase::ReleaseForWait,name,nullptr,mode,duration);
        const auto reply=locks.request(tx.id,name,mode,duration,false,Clock::now()+timeout);
        if(reply.status==Status::Ok) {
            if(duration==Duration::Manual)temporary=Temporary{name,reply.previous};
            event(tx,Phase::WaitGranted,name,nullptr,mode,duration);
        }
        return reply.status;
    }
    void drop_temporary(State& tx,std::optional<Temporary>& temporary) {
        if(temporary){locks.restore(tx.id,temporary->name,temporary->prior);temporary.reset();}
    }
    Status record_x(State& tx,rid_t rid) {
        std::optional<Temporary> unused;
        return wait(tx,{Name::Record,rid},LockMode::X,Duration::Commit,unused);
    }

    // Figure 8: split first, finish propagation, then restart the Insert action.
    // Failure to get the tree latch conditionally drops page latches and retries;
    // this prevents two splitters waiting on each other's latched sibling.
    void split(State& tx,Hold& leaf) {
        std::unique_lock<std::shared_mutex> tree(tree_latch,std::try_to_lock);
        if(!tree.owns_lock()){leaf.release();posc(tx);return;}
        Page* page=leaf.page;
        if(page->retired || page->keys.size()<order-1){leaf.release();return;}
        event(tx,Phase::SplitStarted,key_name(page->keys.front().key),page);
        ++splits;
        if(page==root) {split_root(tx,leaf);return;}
        std::vector<Page*> affected;
        page->sm=true;++page->version;affected.push_back(page);
        Page* right=make_page(true);right->sm=true;right->parent=page->parent;
        const auto middle=page->keys.size()/2;
        right->keys.assign(page->keys.begin()+middle,page->keys.end());page->keys.resize(middle);
        right->prev=page;right->next=page->next;
        page->next=right;affected.push_back(right);
        if(right->next) {Hold sibling(right->next,true,tx.page_latches,max_latches);sibling.page->prev=right;++sibling.page->version;}
        Entry separator=right->keys.front();if(unique)separator.rid=0;
        Page* parent=page->parent;
        leaf.release(); // no upward page-latch acquisition with a child latched
        for(;;) {
            Hold held(parent,true,tx.page_latches,max_latches);
            parent->sm=true;++parent->version;affected.push_back(parent);
            const auto child=std::find(parent->children.begin(),parent->children.end(),page);
            if(child==parent->children.end())throw std::logic_error("missing split child");
            const auto slot=child-parent->children.begin();
            parent->keys.insert(parent->keys.begin()+slot,separator);
            parent->children.insert(child+1,right);right->parent=parent;
            if(parent->keys.size()<order) {held.release();break;}
            if(parent==root){split_root(tx,held);break;}
            Page* new_right=make_page(false);new_right->sm=true;new_right->parent=parent->parent;
            const auto mid=parent->keys.size()/2;separator=parent->keys[mid];
            new_right->keys.assign(parent->keys.begin()+mid+1,parent->keys.end());
            new_right->children.assign(parent->children.begin()+mid+1,parent->children.end());
            for(auto* child_page:new_right->children)child_page->parent=new_right;
            parent->keys.resize(mid);parent->children.resize(mid+1);
            affected.push_back(new_right);page=parent;right=new_right;parent=page->parent;
            held.release();
        }
        for(auto* changed:affected) {Hold held(changed,true,tx.page_latches,max_latches);changed->sm=false;++changed->version;}
        event(tx,Phase::SplitPublished,key_name(separator.key));
    }
    // Root identity does not change (§2.2). Old root contents become children.
    void split_root(State& tx,Hold& held) {
        Page* page=held.page;const bool was_leaf=page->leaf;
        Page* left=make_page(was_leaf);Page* right=make_page(was_leaf);
        left->parent=right->parent=root;
        const auto middle=page->keys.size()/2;
        Entry separator=page->keys[middle];
        if(was_leaf && unique)separator.rid=0;
        left->keys.assign(page->keys.begin(),page->keys.begin()+middle);
        right->keys.assign(page->keys.begin()+middle+(was_leaf?0:1),page->keys.end());
        if(was_leaf) {left->next=right;right->prev=left;}
        else {
            left->children.assign(page->children.begin(),page->children.begin()+middle+1);
            right->children.assign(page->children.begin()+middle+1,page->children.end());
            for(auto* child:left->children)child->parent=left;
            for(auto* child:right->children)child->parent=right;
        }
        page->leaf=false;page->keys={separator};page->children={left,right};page->next=page->prev=nullptr;
        page->sm=false;++page->version;held.release();
        event(tx,Phase::SplitPublished,key_name(separator.key));
    }

    struct Found {Entry entry;Name name;Page* page;std::size_t slot;};
    Found candidate(State& tx,Hold& first,Hold& second,const Entry& probe,bool strict) {
        auto* page=first.page;
        auto position=strict?std::upper_bound(page->keys.begin(),page->keys.end(),probe,less):
                             std::lower_bound(page->keys.begin(),page->keys.end(),probe,less);
        if(position!=page->keys.end())return {*position,key_name(position->key),page,std::size_t(position-page->keys.begin())};
        if(page->next) {
            second=Hold(page->next,false,tx.page_latches,max_latches);
            if(second.page->keys.empty() || second.page->retired)return {{},eof_name(),nullptr,0};
            return {second.page->keys.front(),key_name(second.page->keys.front().key),second.page,0};
        }
        return {{},eof_name(),page,page->keys.size()};
    }
    // Figure 6: conditionally lock under both leaf latches; wait with neither.
    // A manual-duration wait grant is converted to Commit only after revalidation.
    Status fetch_pair(State& tx,Entry probe,bool strict,Entry& result,Cursor* cursor=nullptr) {
        std::optional<Temporary> temporary;
        for(;;) {
            Hold first=search(tx,probe,false),second;
            auto found=candidate(tx,first,second,probe,strict);
            if(!found.page){second.release();first.release();posc(tx);continue;}
            if(cursor && cursor->positioned && cursor->stop==Stop::Equal &&
               (found.name.kind==Name::Eof || found.entry.key!=cursor->current.key)) {
                // Section 5.2's equality stopping exception also applies after
                // VN changes and cursor repositioning, not only the fast path.
                drop_temporary(tx,temporary);cursor->done=true;return Status::NotFound;
            }
            if(temporary && !(temporary->name==found.name))drop_temporary(tx,temporary);
            // Section 5.2: repositioning onto another RID of the current key
            // is already protected by the cursor's commit-duration S lock.
            const bool same_value=cursor && cursor->positioned && found.name.kind!=Name::Eof &&
                found.entry.key==cursor->current.key;
            auto locked=same_value?Reply{Status::Ok,{}}:
                conditional(tx,found.name,LockMode::S,Duration::Commit,found.page);
            if(locked.status!=Status::Ok) {
                const auto page_id=found.page->id,version=found.page->version;
                Page* saved_page=found.page;const bool was_first=saved_page==first.page;
                second.release();first.release();
                drop_temporary(tx,temporary);
                const auto status=wait(tx,found.name,LockMode::S,Duration::Manual,temporary);
                if(status!=Status::Ok)return status;
                ++revalidations;
                // Restart at the root is always safe, including Figure 4's
                // unchanged second page. VN-based ancestor shortcut is optional.
                if(observer)observer({Phase::Revalidate,tx.id,found.name.key,found.name.kind==Name::Eof,page_id,version,0,LockMode::S,Duration::Manual});
                // Section 5.1: unchanged FIRST leaf is a cheap VN revalidation.
                // This shortcut is forbidden for the second leaf (Figure 4).
                Hold revisit(saved_page,false,tx.page_latches,max_latches);
                if(was_first && saved_page->version==version && saved_page->leaf && !saved_page->retired && !saved_page->sm) {
                    const auto promoted=conditional(tx,found.name,LockMode::S,Duration::Commit,saved_page);
                    if(promoted.status!=Status::Ok)throw std::logic_error("manual S lock lost before promotion");
                    temporary.reset();
                    if(found.name.kind==Name::Eof){if(cursor)cursor->done=true;return Status::NotFound;}
                    result=found.entry;
                    if(cursor){cursor->txn=tx.id;cursor->current=result;cursor->page=page_id;cursor->version=version;cursor->slot=found.slot;cursor->positioned=true;}
                    return Status::Ok;
                }
                continue;
            }
            temporary.reset(); // the manual lock was just promoted to Commit
            if(found.name.kind==Name::Eof) {if(cursor)cursor->done=true;return Status::NotFound;}
            result=found.entry;
            if(cursor) {
                cursor->txn=tx.id;cursor->current=result;cursor->page=found.page->id;
                cursor->version=found.page->version;cursor->slot=found.slot;cursor->positioned=true;
            }
            return Status::Ok;
        }
    }

    Status insert(State& tx,Entry entry,bool undo=false) {
        if(!undo) {auto status=record_x(tx,entry.rid);if(status!=Status::Ok)return status;}
        const Entry probe=unique?Entry{entry.key,0,0}:entry;
        for(;;) {
            Hold first=search(tx,probe,true),second;
            auto* leaf=first.page;
            if(leaf->keys.size()>=order-1){split(tx,first);continue;}
            auto pos=std::lower_bound(leaf->keys.begin(),leaf->keys.end(),probe,less);
            auto equal=pos!=leaf->keys.end() && pos->key==entry.key;
            if(equal && (unique || same_pair(*pos,entry))) {
                if(undo)throw std::logic_error("undo insertion found existing pair");
                const auto reply=conditional(tx,key_name(entry.key),LockMode::S,Duration::Commit,leaf);
                if(reply.status==Status::Ok)return Status::Duplicate;
                first.release();std::optional<Temporary> unused;
                const auto status=wait(tx,key_name(entry.key),LockMode::S,Duration::Commit,unused);
                if(status!=Status::Ok)return status;
                    ++revalidations;continue;
            }
            // Same-key instances immediately before the RID insertion point
            // also avoid the next-key request in the nonunique case.
            equal=equal || (pos!=leaf->keys.begin() && (pos-1)->key==entry.key);
            Name next=eof_name();bool need_next=!equal;
            if(need_next) {
                if(pos!=leaf->keys.end())next=key_name(pos->key);
                else if(leaf->next) {
                    second=Hold(leaf->next,false,tx.page_latches,max_latches);
                    if(second.page->keys.empty() || second.page->retired) {second.release();first.release();posc(tx);continue;}
                    next=key_name(second.page->keys.front().key);
                    if(next.key==entry.key && unique && !undo) {
                        const auto reply=conditional(tx,key_name(entry.key),LockMode::S,Duration::Commit,second.page);
                        if(reply.status==Status::Ok)return Status::Duplicate;
                        second.release();first.release();std::optional<Temporary> unused;
                        const auto status=wait(tx,key_name(entry.key),LockMode::S,Duration::Commit,unused);
                        if(status!=Status::Ok)return status;
                        ++revalidations;continue;
                    }
                    if(next.key==entry.key && !unique)need_next=false;
                }
            }
            LockMode mode=LockMode::IX;
            if(!undo && need_next) {
                const auto next_lock=conditional(tx,next,LockMode::IX,Duration::Instant,leaf);
                if(next_lock.status!=Status::Ok) {
                    second.release();first.release();std::optional<Temporary> unused;
                    auto status=wait(tx,next,LockMode::IX,Duration::Instant,unused);
                    if(status!=Status::Ok)return status;
                    ++revalidations;continue;
                }
                if(next_lock.previous && (next_lock.previous->mode==LockMode::S ||
                    next_lock.previous->mode==LockMode::SIX || next_lock.previous->mode==LockMode::X))mode=LockMode::X;
            }
            second.release();
            if(!undo) {
                auto own=conditional(tx,key_name(entry.key),mode,Duration::Commit,leaf);
                if(own.status!=Status::Ok) {
                    first.release();std::optional<Temporary> unused;
                    auto status=wait(tx,key_name(entry.key),mode,Duration::Commit,unused);
                    if(status!=Status::Ok)return status;
                    ++revalidations;continue;
                }
                if(mode==LockMode::X){++replications;event(tx,Phase::Replicated,key_name(entry.key),leaf,mode);}
                // Volatile undo record, reserved before mutation. This is not WAL.
                tx.undo.push_back({true,entry});
            }
            leaf->keys.insert(std::lower_bound(leaf->keys.begin(),leaf->keys.end(),entry,less),entry);
            ++leaf->version;++records;
            if(undo){++undos;event(tx,Phase::UndoDelete,key_name(entry.key),leaf);}
            else {++inserts;event(tx,Phase::Inserted,key_name(entry.key),leaf,mode);}
            return Status::Ok;
        }
    }

    // Figure 10: physically unlink an empty leaf, then propagate upward with
    // no child page latch held. Root identity remains stable; pages are retired.
    void delete_page(State& tx,Hold& first,std::unique_lock<std::shared_mutex>& tree) {
        (void)tree;
        Page* page=first.page;Page* parent=page->parent;
        Page* previous=page->prev;Page* next=page->next;
        page->sm=true;page->retired=true;++page->version;
        if(next){Hold right(next,true,tx.page_latches,max_latches);right.page->prev=previous;++right.page->version;}
        first.release();
        if(previous){Hold left(previous,true,tx.page_latches,max_latches);left.page->next=next;++left.page->version;}
        while(parent) {
            Hold held(parent,true,tx.page_latches,max_latches);parent->sm=true;++parent->version;
            const auto found=std::find(parent->children.begin(),parent->children.end(),page);
            if(found==parent->children.end())throw std::logic_error("missing deleted child");
            const auto slot=std::size_t(found-parent->children.begin());parent->children.erase(found);
            if(!parent->keys.empty())parent->keys.erase(parent->keys.begin()+(slot?slot-1:0));
            if(parent==root && parent->children.empty()) {parent->leaf=true;parent->keys.clear();parent->sm=false;break;}
            if(!parent->children.empty()) {
                if(parent==root)while(!root->leaf && root->children.size()==1) {
                    Page* child=root->children.front();Hold child_hold(child,true,tx.page_latches,max_latches);
                    root->leaf=child->leaf;root->keys=std::move(child->keys);root->children=std::move(child->children);
                    for(auto* grandchild:root->children)grandchild->parent=root;
                    if(root->leaf){root->prev=nullptr;root->next=nullptr;}
                    child->retired=true;child->sm=true;++child->version;++root->version;
                }
                parent->sm=false;break;
            }
            parent->retired=true;page=parent;parent=page->parent;held.release();
        }
        ++page_deletes;event(tx,Phase::PageDeleted,eof_name());
    }

    Status erase(State& tx,Entry probe,bool undo=false) {
        if(!undo){auto status=record_x(tx,probe.rid);if(status!=Status::Ok)return status;}
        std::optional<Temporary> temporary;
        for(;;) {
            Hold first=search(tx,probe,true),second;auto* leaf=first.page;
            auto position=std::lower_bound(leaf->keys.begin(),leaf->keys.end(),probe,less);
            if(position==leaf->keys.end() || !same_pair(*position,probe))return Status::NotFound;
            const auto slot=std::size_t(position-leaf->keys.begin());const Entry removed=*position;
            bool multiple=(slot && leaf->keys[slot-1].key==probe.key) ||
                (slot+1<leaf->keys.size() && leaf->keys[slot+1].key==probe.key);
            Name next=eof_name();
            if(!multiple) {
                if(slot+1<leaf->keys.size())next=key_name(leaf->keys[slot+1].key);
                else if(leaf->next) {
                    second=Hold(leaf->next,false,tx.page_latches,max_latches);
                    if(second.page->keys.empty() || second.page->retired){second.release();first.release();posc(tx);continue;}
                    next=key_name(second.page->keys.front().key);
                    if(next.key==probe.key)multiple=true;
                }
            }
            const bool successor_needed=!multiple;
            if(temporary && (!successor_needed || !(temporary->name==next)))drop_temporary(tx,temporary);
            if(!undo && successor_needed) {
                auto reply=conditional(tx,next,LockMode::X,Duration::Commit,leaf);
                if(reply.status!=Status::Ok) {
                    second.release();first.release();drop_temporary(tx,temporary);
                    auto status=wait(tx,next,LockMode::X,Duration::Manual,temporary);
                    if(status!=Status::Ok)return status;
                    ++revalidations;continue;
                }
                temporary.reset();
            }
            second.release();
            // At the first key of a nonunique leaf, copies might be on the left.
            const auto duration=multiple || (!unique && slot==0)?Duration::Commit:Duration::Instant;
            if(!undo) {
                auto reply=conditional(tx,key_name(probe.key),LockMode::X,duration,leaf);
                if(reply.status!=Status::Ok) {
                    first.release();std::optional<Temporary> unused;
                    auto status=wait(tx,key_name(probe.key),LockMode::X,duration,unused);
                    if(status!=Status::Ok)return status;
                    ++revalidations;continue;
                }
            }
            std::unique_lock<std::shared_mutex> tree;
            if(leaf!=root && leaf->keys.size()==1) {
                tree=std::unique_lock<std::shared_mutex>(tree_latch,std::try_to_lock);
                if(!tree.owns_lock()){first.release();posc(tx);continue;}
            }
            if(!undo)tx.undo.push_back({false,removed});
            leaf->keys.erase(leaf->keys.begin()+slot);++leaf->version;--records;
            if(undo){++undos;event(tx,Phase::UndoInsert,key_name(probe.key),leaf);}
            else {++deletes;event(tx,Phase::Deleted,key_name(probe.key),leaf,LockMode::X,duration);}
            if(tree.owns_lock())delete_page(tx,first,tree);
            return Status::Ok;
        }
    }
};

Index::Transaction::Transaction(std::unique_ptr<State> state):state_(std::move(state)){}
Index::Transaction::~Transaction() {
    if(!state_ || !state_->active)return;
    // Undo requires only page latches; retain all commit locks through undo.
    auto& tx=*state_;
    for(auto it=tx.undo.rbegin();it!=tx.undo.rend();++it) {
        auto status=it->inserted?tx.index->erase(tx,it->entry,true):tx.index->insert(tx,it->entry,true);
        if(status!=Status::Ok)std::terminate();
    }
    tx.index->locks.finish(tx.id);tx.active=false;
}
txnid_t Index::Transaction::id() const {return state_->id;}
bool Index::Transaction::active() const {return state_->active;}
Index::Index(std::size_t order,bool unique,std::chrono::milliseconds timeout):impl_(new Impl(order,unique,timeout)){}
Index::~Index()=default;
std::unique_ptr<Index::Transaction> Index::begin() {
    auto state=std::make_unique<Transaction::State>();state->index=impl_.get();state->id=impl_->next_txn.fetch_add(1);
    return std::unique_ptr<Transaction>(new Transaction(std::move(state)));
}
Status Index::commit(Transaction& transaction) {
    if(!impl_->valid(transaction))return Status::Invalid;
    auto& tx=*transaction.state_;tx.undo.clear();impl_->locks.finish(tx.id);tx.active=false;return Status::Ok;
}
Status Index::abort(Transaction& transaction) {
    if(!impl_->valid(transaction))return Status::Invalid;
    auto& tx=*transaction.state_;
    for(auto it=tx.undo.rbegin();it!=tx.undo.rend();++it) {
        const auto status=it->inserted?impl_->erase(tx,it->entry,true):impl_->insert(tx,it->entry,true);
        if(status!=Status::Ok)throw std::logic_error("logical undo failed");
    }
    tx.undo.clear();impl_->locks.finish(tx.id);tx.active=false;return Status::Ok;
}
Status Index::insert(Transaction& tx,Entry entry) {return impl_->valid(tx)?impl_->insert(*tx.state_,entry):Status::Invalid;}
Status Index::erase(Transaction& tx,idx_key_t key,rid_t rid) {return impl_->valid(tx)?impl_->erase(*tx.state_,{key,rid,0}):Status::Invalid;}
Status Index::fetch(Transaction& transaction,idx_key_t key,Start start,Entry& result) {
    if(!impl_->valid(transaction))return Status::Invalid;
    const Entry probe{key,start==Start::Greater?std::numeric_limits<rid_t>::max():0,0};
    const auto status=impl_->fetch_pair(*transaction.state_,probe,start==Start::Greater,result);
    if(status==Status::Ok && start==Start::Equal && result.key!=key)return Status::NotFound;
    return status;
}
namespace {
bool stopped(idx_key_t key,idx_key_t end,Stop stop) {
    return stop==Stop::Less?key>=end:stop==Stop::LessEqual?key>end:key!=end;
}
}
Status Index::open(Transaction& tx,idx_key_t start,Start kind,idx_key_t end,Stop stop,Cursor& cursor,Entry& result) {
    if(!impl_->valid(tx))return Status::Invalid;
    cursor={};cursor.txn=tx.id();cursor.stop_key=end;cursor.stop=stop;
    Entry probe{start,kind==Start::Greater?std::numeric_limits<rid_t>::max():0,0};
    auto status=impl_->fetch_pair(*tx.state_,probe,kind==Start::Greater,result,&cursor);
    if(status==Status::Ok && ((kind==Start::Equal && result.key!=start) || stopped(result.key,end,stop))) {
        cursor.done=true;return Status::NotFound;
    }
    return status;
}
Status Index::next(Transaction& transaction,Cursor& cursor,Entry& result) {
    if(!impl_->valid(transaction) || cursor.txn!=transaction.id() || !cursor.positioned)return Status::Invalid;
    if(cursor.done)return Status::NotFound;
    if(impl_->unique && cursor.stop==Stop::Equal && cursor.current.key==cursor.stop_key) {cursor.done=true;return Status::NotFound;}
    auto& tx=*transaction.state_;
    // Page pointers never dangle, even after physical page deletion.
    Page* page=nullptr;
    {
        // Directory synchronization is not the tree's SMO latch and is released
        // before taking a page latch. Page addresses remain stable after retire.
        std::lock_guard<std::mutex> directory(impl_->page_directory);
        if(cursor.page && cursor.page<=impl_->pages.size())page=impl_->pages[cursor.page-1].get();
    }
    if(page) {
        Hold first(page,false,tx.page_latches,impl_->max_latches),second;
        if(!page->retired && !page->sm && page->leaf && page->version==cursor.version &&
           cursor.slot<page->keys.size() && same_pair(page->keys[cursor.slot],cursor.current)) {
            // Section 5.2: an unchanged VN makes the saved slot valid. Advance
            // directly instead of searching this leaf for the same key again.
            Impl::Found found{{},eof_name(),page,page->keys.size()};
            const auto slot=cursor.slot+1;
            if(slot<page->keys.size())found={page->keys[slot],key_name(page->keys[slot].key),page,slot};
            else if(page->next) {
                second=Hold(page->next,false,tx.page_latches,impl_->max_latches);
                if(second.page->retired || second.page->sm || second.page->keys.empty())found.page=nullptr;
                else found={second.page->keys.front(),key_name(second.page->keys.front().key),second.page,0};
            }
            if(found.page) {
                // section 5.2: same key value needs no second S lock. An equality
                // scan ending its duplicate cluster need not lock the next key.
                if(cursor.stop==Stop::Equal &&
                   (found.name.kind==Name::Eof || found.entry.key!=cursor.current.key)) {
                    cursor.done=true;return Status::NotFound;
                }
                const auto reply=found.name.kind!=Name::Eof && found.entry.key==cursor.current.key?
                    Reply{Status::Ok,{}}:impl_->conditional(tx,found.name,LockMode::S,Duration::Commit,found.page);
                if(reply.status==Status::Ok) {
                    ++impl_->cursor_fast;impl_->event(tx,Phase::CursorFast,found.name,found.page);
                    if(found.name.kind==Name::Eof || stopped(found.entry.key,cursor.stop_key,cursor.stop)) {
                        cursor.done=true;return Status::NotFound;
                    }
                    result=found.entry;cursor.current=result;cursor.page=found.page->id;
                    cursor.version=found.page->version;cursor.slot=found.slot;return Status::Ok;
                }
            }
        }
    }
    ++impl_->cursor_repositions;impl_->event(tx,Phase::CursorReposition,key_name(cursor.current.key));
    const auto status=impl_->fetch_pair(tx,cursor.current,true,result,&cursor);
    if(status==Status::Ok && stopped(result.key,cursor.stop_key,cursor.stop)) {cursor.done=true;return Status::NotFound;}
    return status;
}
Status Index::scan(Transaction& tx,idx_key_t start,idx_key_t end,std::vector<Entry>& rows) {
    rows.clear();if(start>=end)return Status::Ok;
    Cursor cursor;Entry row;
    auto status=open(tx,start,Start::GreaterEqual,end,Stop::Less,cursor,row);
    while(status==Status::Ok){rows.push_back(row);status=next(tx,cursor,row);}
    return status==Status::NotFound?Status::Ok:status;
}
void Index::load(const std::vector<Entry>& entries) {
    auto tx=begin();
    for(const auto& entry:entries)if(insert(*tx,entry)!=Status::Ok)throw std::invalid_argument("initial load has duplicate entry");
    commit(*tx);
}
std::vector<Entry> Index::inspect() const {
    std::shared_lock<std::shared_mutex> tree(impl_->tree_latch);
    auto* page=impl_->root;while(!page->leaf)page=page->children.front();
    std::vector<Entry> rows;for(;page;page=page->next)rows.insert(rows.end(),page->keys.begin(),page->keys.end());return rows;
}
bool Index::validate() const {
    std::shared_lock<std::shared_mutex> tree(impl_->tree_latch);
    std::size_t count=0,leaf_depth=0;Page* previous=nullptr;bool have_leaf=false;
    auto visit=[&](auto&& self,Page* page,Page* parent,std::size_t depth,std::optional<Entry> low,std::optional<Entry> high)->bool {
        if(page->parent!=parent || page->retired || page->sm || page->keys.size()>=impl_->order ||
           !std::is_sorted(page->keys.begin(),page->keys.end(),less))return false;
        if(page->leaf) {
            if(page!=impl_->root && page->keys.empty())return false;
            if(have_leaf && depth!=leaf_depth)return false;
            have_leaf=true;leaf_depth=depth;
            if(page->prev!=previous || (previous && previous->next!=page))return false;
            for(const auto& entry:page->keys) {
                if((low && less(entry,*low)) || (high && !less(entry,*high)))return false;
                ++count;
            }
            previous=page;return true;
        }
        if(page->children.size()!=page->keys.size()+1)return false;
        for(std::size_t n=0;n<page->children.size();++n)
            if(!self(self,page->children[n],page,depth+1,n?std::optional<Entry>(page->keys[n-1]):low,
                     n<page->keys.size()?std::optional<Entry>(page->keys[n]):high))return false;
        return true;
    };
    return visit(visit,impl_->root,nullptr,0,{},{}) && count==impl_->records && (!previous || !previous->next);
}
std::size_t Index::size() const {return impl_->records;}
std::size_t Index::height() const {
    std::shared_lock<std::shared_mutex> tree(impl_->tree_latch);
    std::size_t height=1;auto* page=impl_->root;while(!page->leaf){++height;page=page->children.front();}return height;
}
Metrics Index::metrics() const {
    Metrics m;m.lock_requests=impl_->locks.requests;m.instant_requests=impl_->locks.instants;m.lock_waits=impl_->locks.waits;
    m.revalidations=impl_->revalidations;m.replications=impl_->replications;m.splits=impl_->splits;m.page_deletes=impl_->page_deletes;
    m.physical_inserts=impl_->inserts;m.physical_deletes=impl_->deletes;m.undo_actions=impl_->undos;
    m.cursor_fast=impl_->cursor_fast;m.cursor_repositions=impl_->cursor_repositions;m.max_page_latches=impl_->max_latches;
    std::lock_guard<std::mutex> held(impl_->locks.mutex);m.live_lock_names=impl_->locks.table.size();m.peak_lock_names=impl_->locks.peak;return m;
}
std::optional<LockMode> Index::held_key(const Transaction& tx,idx_key_t key,bool eof) const {
    return impl_->locks.held(tx.id(),eof?eof_name():key_name(key));
}
void Index::set_observer(std::function<void(const Event&)> observer) {impl_->observer=std::move(observer);}
} // namespace rrl::aries
