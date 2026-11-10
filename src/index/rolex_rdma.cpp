#include "index/rolex_rdma.h"
#include "common/execution_context.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>

namespace rolex::remote {
namespace {
using namespace rrl;
using Clock=std::chrono::steady_clock;
constexpr std::uint64_t kLocked=1;
std::size_t align64(std::size_t n){return (n+63u)&~std::size_t(63u);}
struct Image { std::vector<std::uint64_t> words; std::size_t n{}; Image(std::size_t bytes):words(bytes/sizeof(std::uint64_t)),n(bytes){} void* data(){return words.data();} };
struct Session { rrl::RdmaConnection conn; std::vector<std::uint64_t> values; std::vector<std::uint64_t> keys; bool attached{false}; };
const void*& owner(){return rrl::execution_index_owner();}
Session* current(){return static_cast<Session*>(rrl::execution_index_session());}
}

std::uint64_t seed_fingerprint(const Seed& seed){
  std::uint64_t h=1469598103934665603ULL; auto word=[&](std::uint64_t v){for(int i=0;i<8;++i){h^=v&255;h*=1099511628211ULL;v>>=8;}};
  word(seed.size()); for(auto& p:seed){word(p.first);word(p.second);} return h;
}
std::size_t region_bytes(const RemoteRolexConfig& c){
  if(!c.leaf_capacity || c.leaf_slots!=kLeafSlots) throw std::invalid_argument("invalid ROLEX RDMA capacity");
  return align64(sizeof(RemoteRolexHeader)+c.leaf_capacity*sizeof(RemoteRolexDirectoryEntry)) + c.leaf_capacity*sizeof(RemoteRolexLeaf);
}
struct RemoteRolexMemory::Impl {
  RemoteRolexConfig cfg; Image image;
  explicit Impl(const RemoteRolexConfig& c,const Seed& seed):cfg(c),image(region_bytes(c)){
    if(seed.size()>c.leaf_capacity*kLeafSlots) throw std::invalid_argument("ROLEX seed exceeds leaf capacity");
    auto* h=reinterpret_cast<RemoteRolexHeader*>(image.data()); *h=RemoteRolexHeader{}; h->bytes=image.n; h->leaf_capacity=c.leaf_capacity;
    h->active_leaves=seed.empty()?1:(seed.size()+kLeafSlots-1)/kLeafSlots; h->next_leaf=h->active_leaves; h->live_keys=seed.size();
    h->seed_fingerprint=seed_fingerprint(seed); h->directory_offset=sizeof(RemoteRolexHeader); h->leaves_offset=align64(h->directory_offset+c.leaf_capacity*sizeof(RemoteRolexDirectoryEntry)); h->leaf_bytes=sizeof(RemoteRolexLeaf);
    auto* d=reinterpret_cast<RemoteRolexDirectoryEntry*>(static_cast<std::uint8_t*>(image.data())+h->directory_offset);
    auto* leaves=reinterpret_cast<RemoteRolexLeaf*>(static_cast<std::uint8_t*>(image.data())+h->leaves_offset);
    if(seed.empty()){d[0]={0,UINT64_MAX,0,0}; return;}
    for(std::size_t i=0;i<h->active_leaves;++i){auto& l=leaves[i]; const auto a=i*kLeafSlots,b=std::min(seed.size(),a+kLeafSlots); l.used=b-a; l.prev=i?i-1:0; l.next=i+1<h->active_leaves?i+1:0; for(std::size_t j=a;j<b;++j){l.keys[j-a]=seed[j].first;l.values[j-a]=seed[j].second;} d[i]={l.keys[0],l.keys[l.used-1],i,0};}
  }
  void* data(){return image.data();} std::size_t bytes()const{return image.n;}
};
RemoteRolexMemory::RemoteRolexMemory(const RemoteRolexConfig& c,const Seed& s):impl_(new Impl(c,s)){}
RemoteRolexMemory::~RemoteRolexMemory()=default; void* RemoteRolexMemory::data(){return impl_->data();} std::size_t RemoteRolexMemory::bytes()const{return impl_->bytes();}

struct RolexRdmaIndex::Impl {
  std::vector<std::unique_ptr<Session>> workers; mutable Session admin; RemoteRolexHeader initial{}; std::uint64_t timeout_ms{10000}; std::atomic<bool> failed{false};
  mutable std::atomic<std::uint64_t> ops{0},reads{0},writes{0};
  Session& session()const{if(owner()==this && current()) return *current(); throw std::runtime_error("ROLEX RDMA worker not prepared");}
};

static bool range_end(const rrl::KeyRangeEnt& r, std::uint64_t k){return r.has_end && (k>r.EndKey || (k==r.EndKey && r.end_bound==rrl::Bound::Open));}
static bool rd(RolexRdmaIndex::Impl* x,Session& s,std::uint64_t off,void* p,std::size_t n){auto&m=s.conn.remote_memory();if(off>m.length||n>m.length-off)return false;x->ops++;x->reads+=n;return s.conn.read(m.address+off,m.rkey,p,n);}
static bool wr(RolexRdmaIndex::Impl* x,Session& s,std::uint64_t off,const void* p,std::size_t n){auto&m=s.conn.remote_memory();if(off>m.length||n>m.length-off)return false;x->ops++;x->writes+=n;return s.conn.write(m.address+off,m.rkey,p,n);}
static bool cas(RolexRdmaIndex::Impl* x,Session&s,std::uint64_t off,std::uint64_t a,std::uint64_t b,std::uint64_t& prev){auto&m=s.conn.remote_memory();if(off>m.length||8>m.length-off)return false;x->ops++;return s.conn.compare_swap(m.address+off,m.rkey,a,b,prev);}
static bool faa(RolexRdmaIndex::Impl*x,Session&s,std::uint64_t off,std::uint64_t add,std::uint64_t&prev){auto&m=s.conn.remote_memory();if(off>m.length||8>m.length-off)return false;x->ops++;return s.conn.fetch_add(m.address+off,m.rkey,add,prev);}
static bool header(RolexRdmaIndex::Impl*x,Session&s,RemoteRolexHeader&h){return rd(x,s,0,&h,sizeof(h)) && h.magic==kMagic && h.format==kFormat && h.bytes==s.conn.remote_memory().length && h.leaf_slots==kLeafSlots;}
static std::uint64_t dir_off(const RemoteRolexHeader&h,std::size_t i){return h.directory_offset+i*sizeof(RemoteRolexDirectoryEntry);} static std::uint64_t leaf_off(const RemoteRolexHeader&h,std::size_t i){return h.leaves_offset+i*h.leaf_bytes;}
static bool read_dir(RolexRdmaIndex::Impl*x,Session&s,const RemoteRolexHeader&h,std::vector<RemoteRolexDirectoryEntry>&d){d.resize(h.active_leaves); for(std::size_t i=0;i<d.size();++i)if(!rd(x,s,dir_off(h,i),&d[i],sizeof(d[i])))return false;return true;}
static bool read_leaf(RolexRdmaIndex::Impl*x,Session&s,const RemoteRolexHeader&h,std::size_t n,RemoteRolexLeaf&out){for(int tries=0;tries<1000;++tries){std::uint64_t v=0;if(!rd(x,s,leaf_off(h,n),&v,8))return false;if(v&1){rrl::cooperative_yield();continue;}if(!rd(x,s,leaf_off(h,n),&out,sizeof(out)))return false;if(out.version==v && !(v&1))return true;}return false;}
static bool lock_leaf(RolexRdmaIndex::Impl*x,Session&s,const RemoteRolexHeader&h,std::size_t n,std::uint64_t&v){for(int tries=0;tries<10000;++tries){if(!rd(x,s,leaf_off(h,n),&v,8))return false;if(v&1){rrl::cooperative_yield();continue;}std::uint64_t p=0;if(!cas(x,s,leaf_off(h,n),v,v+1,p))return false;if(p==v)return true;}return false;}
static bool unlock_leaf(RolexRdmaIndex::Impl*x,Session&s,const RemoteRolexHeader&h,std::size_t n,std::uint64_t v){std::uint64_t p=0;return cas(x,s,leaf_off(h,n),v+1,v+2,p)&&p==v+1;}

RolexRdmaIndex::RolexRdmaIndex():impl_(new Impl){} RolexRdmaIndex::~RolexRdmaIndex(){finish_thread();}
bool RolexRdmaIndex::connect(const std::string& host,std::uint16_t port,std::size_t workers,std::uint64_t expected_seed,std::uint64_t timeout_ms){
 if(!workers||!impl_->workers.empty())return false; impl_->timeout_ms=timeout_ms;
 auto open=[&](Session& ss,bool first){if(!ss.conn.connect_client(host,port))return false; RemoteRolexHeader h{}; if(!header(impl_.get(),ss,h))return false; if(first){impl_->initial=h;if(h.seed_fingerprint!=expected_seed)return false;} else if(h.bytes!=impl_->initial.bytes||h.active_leaves>h.leaf_capacity||h.seed_fingerprint!=expected_seed)return false; return true;};
 if(!open(impl_->admin,true)){impl_->failed=true;return false;} for(std::size_t i=0;i<workers;++i){auto ss=std::make_unique<Session>();if(!open(*ss,false)){impl_->failed=true;return false;}impl_->workers.push_back(std::move(ss));} return true;
}
rrl::RC RolexRdmaIndex::init(std::size_t){return impl_->failed?rrl::ERROR:rrl::RCOK;}
void RolexRdmaIndex::prepare_thread(std::size_t w,std::size_t cap){if(owner()||w>=impl_->workers.size())throw std::runtime_error("invalid ROLEX worker");auto&s=*impl_->workers[w];s.values.reserve(cap);s.keys.reserve(cap);s.attached=true;owner()=impl_.get();rrl::execution_index_session()=&s;}
void RolexRdmaIndex::finish_thread()noexcept{if(owner()==impl_.get()){current()->attached=false;rrl::execution_index_session()=nullptr;owner()=nullptr;}}
void RolexRdmaIndex::prepare_write_capacity(std::size_t n){impl_->session().keys.reserve(n);}
std::size_t RolexRdmaIndex::size()const{auto&s=impl_->admin;RemoteRolexHeader h{};if(!header(impl_.get(),s,h))return 0;return h.live_keys;}
bool RolexRdmaIndex::healthy()const{return !impl_->failed.load();}

bool RolexRdmaIndex::lookup(rrl::idx_key_t key,void*&value)const{auto&s=impl_->session();RemoteRolexHeader h{};if(!header(impl_.get(),s,h))return false;std::vector<RemoteRolexDirectoryEntry>d;if(!read_dir(impl_.get(),s,h,d))return false;std::size_t n=0;for(std::size_t i=0;i<d.size();++i){if(key<d[i].first_key){break;}n=i;if(key<=d[i].last_key)break;}RemoteRolexLeaf l{};if(!read_leaf(impl_.get(),s,h,d[n].leaf_number,l))return false;for(std::size_t i=0;i<l.used;++i)if(l.keys[i]==key){s.values.push_back(l.values[i]);value=&s.values.back();return true;}value=nullptr;return false;}
std::vector<rrl::KeyValue> RolexRdmaIndex::scan_range(const rrl::KeyRangeEnt&r)const{std::vector<rrl::KeyValue> out;scan_range_into(r,out);return out;}
void RolexRdmaIndex::scan_range_into(const rrl::KeyRangeEnt&r,std::vector<rrl::KeyValue>&out)const{out.clear();if(r.empty())return;auto&s=impl_->session();RemoteRolexHeader h{};if(!header(impl_.get(),s,h))return;std::vector<RemoteRolexDirectoryEntry>d;if(!read_dir(impl_.get(),s,h,d))return;s.values.clear();s.values.reserve(h.live_keys);for(auto&e:d){if(r.has_start&&e.last_key<r.StartKey)continue;if(r.has_end&&e.first_key>r.EndKey)break;RemoteRolexLeaf l{};if(!read_leaf(impl_.get(),s,h,e.leaf_number,l))return;for(std::size_t i=0;i<l.used;++i){auto k=l.keys[i];if(r.contains(k)&&!range_end(r,k)){s.values.push_back(l.values[i]);out.push_back(rrl::KeyValue{k,&s.values.back()});}}}}
rrl::RC RolexRdmaIndex::insert(rrl::idx_key_t key,void*value){if(!value)return rrl::Abort;auto&s=impl_->session();RemoteRolexHeader h{};if(!header(impl_.get(),s,h))return rrl::Abort;std::vector<RemoteRolexDirectoryEntry>d;if(!read_dir(impl_.get(),s,h,d)||d.empty())return rrl::Abort;std::size_t pos=0;for(std::size_t i=0;i<d.size();++i){if(key<d[i].first_key)break;pos=i;if(key<=d[i].last_key)break;}std::uint64_t ver=0;if(!lock_leaf(impl_.get(),s,h,d[pos].leaf_number,ver))return rrl::Abort;RemoteRolexLeaf l{};if(!rd(impl_.get(),s,leaf_off(h,d[pos].leaf_number),&l,sizeof(l))){unlock_leaf(impl_.get(),s,h,d[pos].leaf_number,ver);return rrl::Abort;}std::size_t slot=0;while(slot<l.used&&l.keys[slot]<key)++slot;if(slot<l.used&&l.keys[slot]==key){l.values[slot]=*static_cast<std::uint64_t*>(value);l.version=ver;bool ok=wr(impl_.get(),s,leaf_off(h,d[pos].leaf_number)+8,reinterpret_cast<std::uint8_t*>(&l)+8,sizeof(l)-8)&&unlock_leaf(impl_.get(),s,h,d[pos].leaf_number,ver);return ok?rrl::RCOK:rrl::Abort;}if(l.used<kLeafSlots){for(std::size_t i=l.used;i>slot;--i){l.keys[i]=l.keys[i-1];l.values[i]=l.values[i-1];}l.keys[slot]=key;l.values[slot]=*static_cast<std::uint64_t*>(value);++l.used;l.version=ver;bool ok=wr(impl_.get(),s,leaf_off(h,d[pos].leaf_number)+8,reinterpret_cast<std::uint8_t*>(&l)+8,sizeof(l)-8);if(ok){RemoteRolexDirectoryEntry updated{l.keys[0],l.keys[l.used-1],d[pos].leaf_number,0};wr(impl_.get(),s,dir_off(h,pos),&updated,sizeof(updated));std::uint64_t p=0;faa(impl_.get(),s,offsetof(RemoteRolexHeader,live_keys),1,p);ok=unlock_leaf(impl_.get(),s,h,d[pos].leaf_number,ver);}return ok?rrl::RCOK:rrl::Abort;}
// Split under leaf lock.
std::uint64_t newno=0;if(!faa(impl_.get(),s,offsetof(RemoteRolexHeader,next_leaf),1,newno)||newno>=h.leaf_capacity){unlock_leaf(impl_.get(),s,h,d[pos].leaf_number,ver);return rrl::Abort;}RemoteRolexLeaf right{};right.version=0;right.prev=d[pos].leaf_number;right.next=l.next;const std::size_t mid=kLeafSlots/2;right.used=l.used-mid;for(std::size_t i=0;i<right.used;++i){right.keys[i]=l.keys[mid+i];right.values[i]=l.values[mid+i];}l.used=mid;l.next=newno;l.version=ver;if(!wr(impl_.get(),s,leaf_off(h,newno),&right,sizeof(right))||!wr(impl_.get(),s,leaf_off(h,d[pos].leaf_number)+8,reinterpret_cast<std::uint8_t*>(&l)+8,sizeof(l)-8)){unlock_leaf(impl_.get(),s,h,d[pos].leaf_number,ver);return rrl::Abort;}std::uint64_t dl=0;for(int i=0;i<10000;++i){if(cas(impl_.get(),s,offsetof(RemoteRolexHeader,directory_lock),0,1,dl)&&dl==0)break;if(i==9999){unlock_leaf(impl_.get(),s,h,d[pos].leaf_number,ver);return rrl::Abort;}rrl::cooperative_yield();}RemoteRolexDirectoryEntry ne{right.keys[0],right.keys[right.used-1],newno,0};for(std::size_t i=h.active_leaves;i>pos+1;--i){RemoteRolexDirectoryEntry tmp;rd(impl_.get(),s,dir_off(h,i-1),&tmp,sizeof(tmp));wr(impl_.get(),s,dir_off(h,i),&tmp,sizeof(tmp));}RemoteRolexDirectoryEntry le{l.keys[0],l.keys[l.used-1],d[pos].leaf_number,0};wr(impl_.get(),s,dir_off(h,pos),&le,sizeof(le));wr(impl_.get(),s,dir_off(h,pos+1),&ne,sizeof(ne));std::uint64_t active=h.active_leaves+1;wr(impl_.get(),s,offsetof(RemoteRolexHeader,active_leaves),&active,8);std::uint64_t dv=h.directory_version+2;wr(impl_.get(),s,offsetof(RemoteRolexHeader,directory_version),&dv,8);cas(impl_.get(),s,offsetof(RemoteRolexHeader,directory_lock),1,0,dl);bool placed=false;if(key>=ne.first_key){auto target=right;std::size_t z=0;while(z<target.used&&target.keys[z]<key)++z;if(z<target.used&&target.keys[z]==key)target.values[z]=*static_cast<std::uint64_t*>(value);else if(target.used<kLeafSlots){for(std::size_t j=target.used;j>z;--j){target.keys[j]=target.keys[j-1];target.values[j]=target.values[j-1];}target.keys[z]=key;target.values[z]=*static_cast<std::uint64_t*>(value);++target.used;}target.version=0;wr(impl_.get(),s,leaf_off(h,newno),&target,sizeof(target));RemoteRolexDirectoryEntry updated{target.keys[0],target.keys[target.used-1],newno,0};wr(impl_.get(),s,dir_off(h,pos+1),&updated,sizeof(updated));placed=true;}if(!placed){if(l.used<kLeafSlots){std::size_t z=0;while(z<l.used&&l.keys[z]<key)++z;for(std::size_t j=l.used;j>z;--j){l.keys[j]=l.keys[j-1];l.values[j]=l.values[j-1];}l.keys[z]=key;l.values[z]=*static_cast<std::uint64_t*>(value);++l.used;}l.version=ver;wr(impl_.get(),s,leaf_off(h,d[pos].leaf_number)+8,reinterpret_cast<std::uint8_t*>(&l)+8,sizeof(l)-8);}faa(impl_.get(),s,offsetof(RemoteRolexHeader,live_keys),1,dl);unlock_leaf(impl_.get(),s,h,d[pos].leaf_number,ver);return rrl::RCOK;}
rrl::RC RolexRdmaIndex::erase(rrl::idx_key_t key){auto&s=impl_->session();RemoteRolexHeader h{};if(!header(impl_.get(),s,h))return rrl::Abort;std::vector<RemoteRolexDirectoryEntry>d;if(!read_dir(impl_.get(),s,h,d)||d.empty())return rrl::Abort;std::size_t p=0;for(std::size_t i=0;i<d.size();++i){if(key<d[i].first_key)break;p=i;if(key<=d[i].last_key)break;}std::uint64_t ver=0;if(!lock_leaf(impl_.get(),s,h,d[p].leaf_number,ver))return rrl::Abort;RemoteRolexLeaf l{};if(!rd(impl_.get(),s,leaf_off(h,d[p].leaf_number),&l,sizeof(l))){unlock_leaf(impl_.get(),s,h,d[p].leaf_number,ver);return rrl::Abort;}std::size_t i=0;while(i<l.used&&l.keys[i]!=key)++i;if(i==l.used){unlock_leaf(impl_.get(),s,h,d[p].leaf_number,ver);return rrl::Abort;}for(;i+1<l.used;++i){l.keys[i]=l.keys[i+1];l.values[i]=l.values[i+1];}--l.used;l.version=ver;bool ok=wr(impl_.get(),s,leaf_off(h,d[p].leaf_number)+8,reinterpret_cast<std::uint8_t*>(&l)+8,sizeof(l)-8)&&unlock_leaf(impl_.get(),s,h,d[p].leaf_number,ver);if(ok){std::uint64_t prev=0;faa(impl_.get(),s,offsetof(RemoteRolexHeader,live_keys),static_cast<std::uint64_t>(-1),prev);}return ok?rrl::RCOK:rrl::Abort;}
rrl::RC RolexRdmaIndex::apply_batch(const std::vector<rrl::IndexMutation>&m){for(auto&c:m){auto rc=c.remove?erase(c.key):insert(c.key,const_cast<std::uint64_t*>(c.value));if(rc!=rrl::RCOK&&!(c.remove&&rc==rrl::Abort))return rc;}return rrl::RCOK;}
bool RolexRdmaIndex::key_neighbors(rrl::idx_key_t key,rrl::IndexKeyNeighbors&o)const{o={};std::vector<rrl::idx_key_t>ks;if(!key_range_into(rrl::KeyRangeEnt::from_left(0),ks))return false;auto it=std::lower_bound(ks.begin(),ks.end(),key);o.present=it!=ks.end()&&*it==key;if(it!=ks.begin()){o.has_predecessor=true;o.predecessor=*(it-1);}if(it!=ks.end()&&( !o.present)){o.has_successor=true;o.successor=*it;}else if(o.present&&it+1!=ks.end()){o.has_successor=true;o.successor=*(it+1);}return true;}
bool RolexRdmaIndex::key_range_into(const rrl::KeyRangeEnt&r,std::vector<rrl::idx_key_t>&out)const{std::vector<rrl::KeyValue> kv;scan_range_into(r,kv);out.clear();for(auto&x:kv)out.push_back(x.key);return true;}
bool RolexRdmaIndex::validate()const{auto&s=impl_->admin;RemoteRolexHeader h{};if(!header(impl_.get(),s,h)||!h.active_leaves||h.active_leaves>h.next_leaf)return false;std::vector<RemoteRolexDirectoryEntry>d;if(!read_dir(impl_.get(),s,h,d))return false;std::uint64_t count=0;rrl::idx_key_t prev=0;bool first=true;for(auto&e:d){RemoteRolexLeaf l{};if(!read_leaf(impl_.get(),s,h,e.leaf_number,l)||!l.used||l.keys[0]!=e.first_key||l.keys[l.used-1]!=e.last_key)return false;for(std::size_t i=0;i<l.used;++i){if(!first&&l.keys[i]<=prev)return false;first=false;prev=l.keys[i];++count;}}return count==h.live_keys;}
std::uint64_t RolexRdmaIndex::rdma_operations()const{return impl_->ops.load();} std::uint64_t RolexRdmaIndex::read_bytes()const{return impl_->reads.load();} std::uint64_t RolexRdmaIndex::write_bytes()const{return impl_->writes.load();}
}
