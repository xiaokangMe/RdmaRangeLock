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
    struct RidValue {rid_t rid;std::uint64_t value;};
    struct Cluster {idx_key_t key;std::vector<RidValue> rids;};
    std::uint64_t id{0};std::atomic<std::uint64_t> version{1};
    bool leaf{true},sm{false},retired{false};
    std::vector<Entry> keys; // Internal high keys; leaves store key-map clusters below.
    std::vector<Cluster> clusters;
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
    struct PathNode {Page* page;std::uint64_t version;};
    using SearchPath=std::vector<PathNode>;
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
    std::size_t leaf_size(const Page* page) const {
        std::size_t count=0;for(const auto& cluster:page->clusters)count+=cluster.rids.size();return count;
    }
    std::vector<Entry> leaf_entries(const Page* page) const {
        std::vector<Entry> entries;entries.reserve(leaf_size(page));
        for(const auto& cluster:page->clusters)
            for(const auto& rid:cluster.rids)entries.push_back({cluster.key,rid.rid,rid.value});
        return entries;
    }
    void assign_leaf(Page* page,const std::vector<Entry>& entries) {
        page->keys.clear();page->clusters.clear();
        std::size_t cluster_count=0;idx_key_t last=0;bool have_last=false;
        for(const auto& entry:entries)if(!have_last || entry.key!=last) {
            ++cluster_count;last=entry.key;have_last=true;
        }
        page->clusters.reserve(cluster_count);
        for(const auto& entry:entries) {
            if(page->clusters.empty() || page->clusters.back().key!=entry.key)
                page->clusters.push_back({entry.key,{}});
            page->clusters.back().rids.push_back({entry.rid,entry.value});
        }
    }
    Entry leaf_at(const Page* page,std::size_t key_slot,std::size_t rid_slot) const {
        const auto& cluster=page->clusters.at(key_slot);const auto& rid=cluster.rids.at(rid_slot);
        return {cluster.key,rid.rid,rid.value};
    }
    void event(State& tx,Phase phase,Name name,Page* p=nullptr,LockMode mode=LockMode::S,Duration d=Duration::Commit) {
        if(observer)observer({phase,tx.id,name.key,name.kind==Name::Eof,p?p->id:0,
            p?p->version.load(std::memory_order_acquire):0,tx.page_latches,mode,d});
    }
    bool valid(const Transaction& tx) const {return tx.state_ && tx.state_->index==this && tx.state_->active;}
    void posc(State& tx) {
        if(tx.page_latches)throw std::logic_error("wait for POSC while holding a page latch");
        std::shared_lock<std::shared_mutex> wait(tree_latch);
    }
    // Figure 5: parent is held until the child has its appropriate S/X latch.
    // On re-entry, saved VNs permit restart at the deepest unchanged ancestor.
    Hold search(State& tx,const Entry& probe,bool write,const SearchPath* hint=nullptr,SearchPath* observed=nullptr) {
        for(;;) {
            Page* start=root;std::size_t start_depth=0;bool use_hint=false;
            std::optional<std::shared_lock<std::shared_mutex>> path_guard;
            if(hint && hint->size()>1 && (*hint)[0].page==root &&
               root->version.load(std::memory_order_acquire)==(*hint)[0].version) {
                path_guard.emplace(tree_latch);
                for(std::size_t n=1;n+1<hint->size();++n) {
                    if((*hint)[n].page->version.load(std::memory_order_acquire)!=(*hint)[n].version)break;
                    start=(*hint)[n].page;start_depth=n;
                }
                use_hint=true;
            }
            Hold current(start,false,tx.page_latches,max_latches);
            if(start==root && root->leaf && write) {
                current.release();current=Hold(root,true,tx.page_latches,max_latches);
                if(!root->leaf)continue;
            }
            if(start!=root && current.page->version.load(std::memory_order_acquire)!=(*hint)[start_depth].version) {
                current.release();path_guard.reset();hint=nullptr;start=root;start_depth=0;use_hint=false;
                continue;
            }
            if(observed) {
                observed->clear();
                if(use_hint)observed->insert(observed->end(),hint->begin(),hint->begin()+start_depth+1);
                else observed->push_back({current.page,current.page->version.load(std::memory_order_acquire)});
            }
            if(use_hint && start_depth>0)event(tx,Phase::SearchRestart,key_name(probe.key),current.page);
            bool restart=false;
            for(;;) {
                Page* page=current.page;
                if(page->sm || page->retired) {current.release();posc(tx);restart=true;break;}
                if(page->leaf){event(tx,Phase::SearchLeaf,key_name(probe.key),page);return current;}
                Page* child=nullptr;
                const auto depth=observed?observed->size()-1:0;
                if(use_hint && observed && depth+1<hint->size() &&
                   (*hint)[depth].page==page &&
                   (*hint)[depth].version==page->version.load(std::memory_order_acquire)) {
                    child=(*hint)[depth+1].page;
                    event(tx,Phase::SearchHint,key_name(probe.key),child);
                } else {
                    use_hint=false;
                    const auto slot=std::upper_bound(page->keys.begin(),page->keys.end(),probe,less)-page->keys.begin();
                    child=page->children[slot];
                }
                // All nonroot page types remain fixed for their entire lifetime.
                Hold next(child,write && child->leaf,tx.page_latches,max_latches);
                if(observed)observed->push_back({next.page,next.page->version.load(std::memory_order_acquire)});
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
        const auto used=page->leaf?leaf_size(page):page->keys.size();
        if(page->retired || used<(page->leaf?order-1:order)){leaf.release();return;}
        const auto first_key=page->leaf?page->clusters.front().key:page->keys.front().key;
        event(tx,Phase::SplitStarted,key_name(first_key),page);
        ++splits;
        if(page==root) {split_root(tx,leaf);return;}
        std::vector<Page*> affected;
        page->sm=true;++page->version;affected.push_back(page);
        Page* right=make_page(true);right->sm=true;right->parent=page->parent;
        const auto original_next=page->next;
        const auto entries=leaf_entries(page);const auto middle=entries.size()/2;
        assign_leaf(right,std::vector<Entry>(entries.begin()+middle,entries.end()));
        assign_leaf(page,std::vector<Entry>(entries.begin(),entries.begin()+middle));
        right->prev=page;right->next=page->next;
        page->next=right;affected.push_back(right);
        right->next=original_next;
        if(right->next) {Hold sibling(right->next,true,tx.page_latches,max_latches);sibling.page->prev=right;++sibling.page->version;}
        Entry separator=leaf_entries(right).front();if(unique)separator.rid=0;
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
    // Section 2.2: copy the old root contents into a new child page, then split
    // that copied page. The original root page remains at the same address.
    void split_root(State& tx,Hold& held) {
        Page* page=held.page;const bool was_leaf=page->leaf;
        page->sm=true;++page->version;
        Page* copied=make_page(was_leaf);
        Hold copied_hold(copied,true,tx.page_latches,max_latches);
        copied->keys=page->keys;copied->clusters=page->clusters;copied->children=page->children;
        copied->parent=root;copied->sm=true;copied->version=page->version.load(std::memory_order_acquire)+1;
        copied->prev=was_leaf?nullptr:page->prev;
        copied->next=was_leaf?nullptr:page->next;
        for(auto* child:copied->children)child->parent=copied;
        const auto root_first=was_leaf?page->clusters.front().key:page->keys.front().key;
        event(tx,Phase::RootCopied,key_name(root_first),copied);

        // The copied page is the page that is split. Retain its identity for
        // the left half and allocate only the new right sibling.
        Page* right=make_page(was_leaf);right->parent=root;right->sm=true;
        copied->sm=true;++copied->version;
        Entry separator;
        Page* old_next=copied->next;
        if(was_leaf) {
            const auto entries=leaf_entries(copied);const auto middle=entries.size()/2;
            separator=entries[middle];
            assign_leaf(copied,std::vector<Entry>(entries.begin(),entries.begin()+middle));
            assign_leaf(right,std::vector<Entry>(entries.begin()+middle,entries.end()));
            if(unique)separator.rid=0;
            copied->next=right;right->prev=copied;
        }
        else {
            const auto middle=copied->keys.size()/2;separator=copied->keys[middle];
            right->keys.assign(copied->keys.begin()+middle+1,copied->keys.end());
            right->children.assign(copied->children.begin()+middle+1,copied->children.end());
            copied->keys.resize(middle);
            copied->children.resize(middle+1);
            for(auto* child:right->children)child->parent=right;
        }
        right->next=old_next;
        copied->next=right;
        copied_hold.release();

        page->leaf=false;page->keys={separator};page->clusters.clear();page->children={copied,right};page->next=page->prev=nullptr;
        page->sm=false;++page->version;
        copied->sm=false;++copied->version;right->sm=false;++right->version;
        event(tx,Phase::SplitPublished,key_name(separator.key),page);
        held.release();
    }

    struct Found {Entry entry;Name name;Page* page;std::size_t slot,rid_slot;};
    Found candidate(State& tx,Hold& first,Hold& second,const Entry& probe,bool strict) {
        auto* page=first.page;
        auto cluster=std::lower_bound(page->clusters.begin(),page->clusters.end(),probe.key,
            [](const Page::Cluster& item,idx_key_t key){return item.key<key;});
        if(cluster!=page->clusters.end() && cluster->key==probe.key) {
            auto rid=strict?std::upper_bound(cluster->rids.begin(),cluster->rids.end(),probe.rid,
                [](rid_t value,const Page::RidValue& item){return value<item.rid;}):
                std::lower_bound(cluster->rids.begin(),cluster->rids.end(),probe.rid,
                [](const Page::RidValue& item,rid_t value){return item.rid<value;});
            if(rid!=cluster->rids.end()) {
                const auto key_slot=std::size_t(cluster-page->clusters.begin());
                const auto rid_slot=std::size_t(rid-cluster->rids.begin());
                const auto entry=leaf_at(page,key_slot,rid_slot);
                return {entry,key_name(entry.key),page,key_slot,rid_slot};
            }
            ++cluster;
        }
        if(cluster!=page->clusters.end()) {
            const auto key_slot=std::size_t(cluster-page->clusters.begin());
            const auto entry=leaf_at(page,key_slot,0);
            return {entry,key_name(entry.key),page,key_slot,0};
        }
        if(page->next) {
            second=Hold(page->next,false,tx.page_latches,max_latches);
            if(second.page->clusters.empty() || second.page->retired)return {{},eof_name(),nullptr,0,0};
            const auto entry=leaf_at(second.page,0,0);
            return {entry,key_name(entry.key),second.page,0,0};
        }
        return {{},eof_name(),page,page->clusters.size(),0};
    }
    // Figure 6: conditionally lock under both leaf latches; wait with neither.
    // A manual-duration wait grant is converted to Commit only after revalidation.
    Status fetch_pair(State& tx,Entry probe,bool strict,Entry& result,Cursor* cursor=nullptr) {
        std::optional<Temporary> temporary;
        SearchPath hint,path;bool has_hint=false;
        for(;;) {
            Hold first=search(tx,probe,false,has_hint?&hint:nullptr,&path),second;
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
                const auto page_id=found.page->id;
                const auto version=found.page->version.load(std::memory_order_acquire);
                Page* saved_page=found.page;const bool was_first=saved_page==first.page;
                hint=path;has_hint=true;
                second.release();first.release();
                drop_temporary(tx,temporary);
                const auto status=wait(tx,found.name,LockMode::S,Duration::Manual,temporary);
                if(status!=Status::Ok)return status;
                ++revalidations;
                // Re-search the first-leaf candidate even when the lock came
                // from the second leaf (Figure 4); search() uses saved VNs to
                // resume at the deepest unchanged ancestor where possible.
                if(observer)observer({Phase::Revalidate,tx.id,found.name.key,found.name.kind==Name::Eof,page_id,version,0,LockMode::S,Duration::Manual});
                // Section 5.1: unchanged FIRST leaf is a cheap VN revalidation.
                // This shortcut is forbidden for the second leaf (Figure 4).
                Hold revisit(saved_page,false,tx.page_latches,max_latches);
                if(was_first && saved_page->version.load(std::memory_order_acquire)==version && saved_page->leaf && !saved_page->retired && !saved_page->sm) {
                    const auto promoted=conditional(tx,found.name,LockMode::S,Duration::Commit,saved_page);
                    if(promoted.status!=Status::Ok)throw std::logic_error("manual S lock lost before promotion");
                    temporary.reset();
                    if(found.name.kind==Name::Eof){if(cursor)cursor->done=true;return Status::NotFound;}
                    result=found.entry;
                    if(cursor){cursor->txn=tx.id;cursor->current=result;cursor->page=page_id;cursor->version=version;cursor->slot=found.slot;cursor->rid_slot=found.rid_slot;cursor->positioned=true;}
                    return Status::Ok;
                }
                continue;
            }
            temporary.reset(); // the manual lock was just promoted to Commit
            if(found.name.kind==Name::Eof) {if(cursor)cursor->done=true;return Status::NotFound;}
            result=found.entry;
            if(cursor) {
                cursor->txn=tx.id;cursor->current=result;cursor->page=found.page->id;
                cursor->version=found.page->version.load(std::memory_order_acquire);cursor->slot=found.slot;cursor->rid_slot=found.rid_slot;cursor->positioned=true;
            }
            return Status::Ok;
        }
    }

    Status insert(State& tx,Entry entry,bool undo=false) {
        if(!undo) {auto status=record_x(tx,entry.rid);if(status!=Status::Ok)return status;}
        const Entry probe=unique?Entry{entry.key,0,0}:entry;
        SearchPath hint,path;bool has_hint=false;
        for(;;) {
            Hold first=search(tx,probe,true,has_hint?&hint:nullptr,&path),second;
            auto* leaf=first.page;
            if(leaf_size(leaf)>=order-1){split(tx,first);continue;}
            auto entries=leaf_entries(leaf);
            auto pos=std::lower_bound(entries.begin(),entries.end(),probe,less);
            auto equal=pos!=entries.end() && pos->key==entry.key;
            if(equal && (unique || same_pair(*pos,entry))) {
                if(undo)throw std::logic_error("undo insertion found existing pair");
                const auto reply=conditional(tx,key_name(entry.key),LockMode::S,Duration::Commit,leaf);
                if(reply.status==Status::Ok)return Status::Duplicate;
                hint=path;has_hint=true;
                first.release();std::optional<Temporary> unused;
                const auto status=wait(tx,key_name(entry.key),LockMode::S,Duration::Commit,unused);
                if(status!=Status::Ok)return status;
                    ++revalidations;continue;
            }
            // Same-key instances immediately before the RID insertion point
            // also avoid the next-key request in the nonunique case.
            equal=equal || (pos!=entries.begin() && (pos-1)->key==entry.key);
            Name next=eof_name();bool need_next=!equal;
            if(need_next) {
                if(pos!=entries.end())next=key_name(pos->key);
                else if(leaf->next) {
                    second=Hold(leaf->next,false,tx.page_latches,max_latches);
                    if(second.page->clusters.empty() || second.page->retired) {second.release();first.release();posc(tx);continue;}
                    next=key_name(second.page->clusters.front().key);
                    if(next.key==entry.key && unique && !undo) {
                        const auto reply=conditional(tx,key_name(entry.key),LockMode::S,Duration::Commit,second.page);
                        if(reply.status==Status::Ok)return Status::Duplicate;
                        hint=path;has_hint=true;
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
                    hint=path;has_hint=true;
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
                    hint=path;has_hint=true;
                    first.release();std::optional<Temporary> unused;
                    auto status=wait(tx,key_name(entry.key),mode,Duration::Commit,unused);
                    if(status!=Status::Ok)return status;
                    ++revalidations;continue;
                }
                if(mode==LockMode::X){++replications;event(tx,Phase::Replicated,key_name(entry.key),leaf,mode);}
                // Volatile undo record, reserved before mutation. This is not WAL.
                tx.undo.push_back({true,entry});
            }
            entries.insert(std::lower_bound(entries.begin(),entries.end(),entry,less),entry);
            assign_leaf(leaf,entries);
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
            if(parent==root && parent->children.empty()) {parent->leaf=true;parent->keys.clear();parent->clusters.clear();parent->sm=false;break;}
            if(!parent->children.empty()) {
                if(parent==root)while(!root->leaf && root->children.size()==1) {
                    Page* child=root->children.front();Hold child_hold(child,true,tx.page_latches,max_latches);
                    root->leaf=child->leaf;root->keys=std::move(child->keys);root->clusters=std::move(child->clusters);root->children=std::move(child->children);
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
        SearchPath hint,path;bool has_hint=false;
        for(;;) {
            Hold first=search(tx,probe,true,has_hint?&hint:nullptr,&path),second;auto* leaf=first.page;
            auto entries=leaf_entries(leaf);
            auto position=std::lower_bound(entries.begin(),entries.end(),probe,less);
            if(position==entries.end() || !same_pair(*position,probe))return Status::NotFound;
            const auto slot=std::size_t(position-entries.begin());const Entry removed=*position;
            bool multiple=(slot && entries[slot-1].key==probe.key) ||
                (slot+1<entries.size() && entries[slot+1].key==probe.key);
            Name next=eof_name();
            if(!multiple) {
                if(slot+1<entries.size())next=key_name(entries[slot+1].key);
                else if(leaf->next) {
                    second=Hold(leaf->next,false,tx.page_latches,max_latches);
                    if(second.page->clusters.empty() || second.page->retired){second.release();first.release();posc(tx);continue;}
                    next=key_name(second.page->clusters.front().key);
                    if(next.key==probe.key)multiple=true;
                }
            }
            const bool successor_needed=!multiple;
            if(temporary && (!successor_needed || !(temporary->name==next)))drop_temporary(tx,temporary);
            if(!undo && successor_needed) {
                auto reply=conditional(tx,next,LockMode::X,Duration::Commit,leaf);
                if(reply.status!=Status::Ok) {
                    hint=path;has_hint=true;
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
                    hint=path;has_hint=true;
                    first.release();std::optional<Temporary> unused;
                    auto status=wait(tx,key_name(probe.key),LockMode::X,duration,unused);
                    if(status!=Status::Ok)return status;
                    ++revalidations;continue;
                }
            }
            std::unique_lock<std::shared_mutex> tree;
            if(leaf!=root && leaf_size(leaf)==1) {
                tree=std::unique_lock<std::shared_mutex>(tree_latch,std::try_to_lock);
                if(!tree.owns_lock()){first.release();posc(tx);continue;}
            }
            if(!undo)tx.undo.push_back({false,removed});
            entries.erase(entries.begin()+slot);assign_leaf(leaf,entries);++leaf->version;--records;
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
            if(!page->retired && !page->sm && page->leaf && page->version.load(std::memory_order_acquire)==cursor.version &&
               cursor.slot<page->clusters.size() && cursor.rid_slot<page->clusters[cursor.slot].rids.size() &&
               same_pair(impl_->leaf_at(page,cursor.slot,cursor.rid_slot),cursor.current)) {
                // Section 5.2: continue by the saved key-map slot and RID
                // ordinal; do not search the current key again.
                Impl::Found found{{},eof_name(),page,page->clusters.size(),0};
                const auto& cluster=page->clusters[cursor.slot];
                if(cursor.rid_slot+1<cluster.rids.size()) {
                    const auto rid_slot=cursor.rid_slot+1;
                    const auto entry=impl_->leaf_at(page,cursor.slot,rid_slot);
                    found={entry,key_name(entry.key),page,cursor.slot,rid_slot};
                } else if(cursor.slot+1<page->clusters.size()) {
                    const auto key_slot=cursor.slot+1;
                    const auto entry=impl_->leaf_at(page,key_slot,0);
                    found={entry,key_name(entry.key),page,key_slot,0};
                } else if(page->next) {
                    second=Hold(page->next,false,tx.page_latches,impl_->max_latches);
                    if(second.page->retired || second.page->sm || second.page->clusters.empty())found.page=nullptr;
                    else {
                        const auto entry=impl_->leaf_at(second.page,0,0);
                        found={entry,key_name(entry.key),second.page,0,0};
                    }
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
                    cursor.version=found.page->version.load(std::memory_order_acquire);cursor.slot=found.slot;cursor.rid_slot=found.rid_slot;return Status::Ok;
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
    std::vector<Entry> rows;for(;page;page=page->next) {
        auto entries=impl_->leaf_entries(page);rows.insert(rows.end(),entries.begin(),entries.end());
    }return rows;
}
bool Index::validate() const {
    std::shared_lock<std::shared_mutex> tree(impl_->tree_latch);
    std::size_t count=0,leaf_depth=0;Page* previous=nullptr;bool have_leaf=false;
    auto visit=[&](auto&& self,Page* page,Page* parent,std::size_t depth,std::optional<Entry> low,std::optional<Entry> high)->bool {
        if(page->parent!=parent || page->retired || page->sm ||
           (page->leaf?impl_->leaf_size(page):page->keys.size())>=impl_->order)return false;
        if(page->leaf) {
            if(!page->keys.empty() || (page!=impl_->root && page->clusters.empty()))return false;
            if(have_leaf && depth!=leaf_depth)return false;
            have_leaf=true;leaf_depth=depth;
            if(page->prev!=previous || (previous && previous->next!=page))return false;
            auto entries=impl_->leaf_entries(page);
            if(!std::is_sorted(entries.begin(),entries.end(),less))return false;
            for(const auto& cluster:page->clusters) {
                if(cluster.rids.empty())return false;
                if(!std::is_sorted(cluster.rids.begin(),cluster.rids.end(),
                    [](const Page::RidValue& a,const Page::RidValue& b){return a.rid<b.rid;}))return false;
            }
            for(const auto& entry:entries) {
                if((low && less(entry,*low)) || (high && !less(entry,*high)))return false;
                ++count;
            }
            previous=page;return true;
        }
        if(!page->clusters.empty() || !std::is_sorted(page->keys.begin(),page->keys.end(),less))return false;
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
