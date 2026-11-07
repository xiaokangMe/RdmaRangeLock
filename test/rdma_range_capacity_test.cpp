#include "lock/remote_lock_manager.h"
#include <cstring>
#include <cstdlib>
#include <iostream>
using namespace rrl;
void require(bool b,const char* m){if(!b)throw std::runtime_error(m);}
int main(int argc,char** argv){try {
    const auto layout=remote_lock_layout(32768);
    require(valid_remote_lock_header(layout,layout.bytes),"dynamic layout");
    auto bad=layout;bad.version=1;require(!valid_remote_lock_header(bad,bad.bytes),"reject old layout");
    bad=layout;++bad.shard_stride;require(!valid_remote_lock_header(bad,bad.bytes),"reject bad stride");
    require(!valid_remote_lock_header(layout,layout.bytes-1),"reject short region");
    if(argc==1)return 0;
    const auto* h=std::getenv("RRL_TEST_RDMA_HOST");const auto* p=std::getenv("RRL_TEST_RDMA_PORT");
    require(h && p,"test service environment");const auto port=static_cast<std::uint16_t>(std::stoul(p));
    RdmaConnection raw;require(raw.connect_client(h,port),"raw probe");const auto mem=raw.remote_memory();
    if(std::string(argv[1])=="--reject-range") {
        std::uint64_t before[16]{},after[16]{};
        require(raw.read(mem.address,mem.rkey,before,sizeof(before)),"read foreign header");
        RemoteLockManager wrong;require(!wrong.connect(h,port),"reject foreign lock backend");
        require(raw.read(mem.address,mem.rkey,after,sizeof(after)) && std::memcmp(before,after,sizeof(before))==0,"foreign header unchanged");
        return 0;
    }
    RemoteLockHeader header{};
    require(raw.read(mem.address,mem.rkey,&header,sizeof(header)) && valid_remote_lock_header(header,mem.length),"range header");
    RemoteLockManager locks;require(locks.connect(h,port),"dynamic client");
    require(locks.diagnostics().slots_per_shard==header.slots_per_shard,"client discovers capacity");
    constexpr std::size_t hot=kRemoteLockShards-1;
    const auto base=mem.address+sizeof(header)+hot*header.shard_stride;
    auto count=[&](std::size_t shard){
        std::uint64_t n=0;
        require(raw.read(mem.address+sizeof(header)+shard*header.shard_stride+offsetof(RemoteLockShard,active_slots),mem.rkey,&n,8),"read count");
        return n;
    };
    auto publish_count=[&](std::uint64_t n){require(raw.write(base+offsetof(RemoteLockShard,active_slots),mem.rkey,&n,8),"fixture count");};
    std::vector<RemoteLockSlot> fixture(header.slots_per_shard);
    for(auto& slot:fixture)encode_remote_slot(slot,0,999999,LockMode::S,LockKind::Record,KeyRangeEnt::point(hot*64));
    for(std::size_t off=0;off<fixture.size()*sizeof(RemoteLockSlot);) {
        const auto n=std::min<std::size_t>(1u<<20,fixture.size()*sizeof(RemoteLockSlot)-off);
        require(raw.write(base+offsetof(RemoteLockShard,slots)+off,mem.rkey,reinterpret_cast<char*>(fixture.data())+off,n),"fixture chunk");
        off+=n;
    }
    publish_count(header.slots_per_shard);const auto point=KeyRangeEnt::point(hot*64);
    require(locks.lock(1,LockMode::S,point,LockKind::Record)==AbortCapacity,"single capacity");
    require(locks.lock_batch(1,LockMode::S,{KeyRangeEnt::point(0),point},LockKind::Record)==AbortCapacity,"batch capacity atomicity");
    require(count(0)==0 && count(hot)==header.slots_per_shard,"no partial publication");
    auto diag=locks.diagnostics();
    require(diag.slot_peaks[hot]==header.slots_per_shard && diag.capacity_required[hot]==header.slots_per_shard+1,"per-shard pressure diagnostics");
    publish_count(header.slots_per_shard-1);
    require(locks.lock(1,LockMode::S,point,LockKind::Record)==RCOK,"last available slot");
    require(locks.lock(1,LockMode::S,point,LockKind::Record)==RCOK && count(hot)==header.slots_per_shard,"idempotent at full");
    require(locks.unlock_all(1)==RCOK && count(hot)==header.slots_per_shard-1,"release preserves other holders");
    publish_count(0);locks.reset_slot_diagnostics();
    require(locks.diagnostics().slot_peaks[hot]==0 && locks.diagnostics().capacity_required[hot]==0,"warmup peaks reset");
    require(locks.lock(2,LockMode::X,point,LockKind::Record)==RCOK && locks.unlock_all(2)==RCOK,"reuse after capacity");
    require(locks.diagnostics().slot_peaks[hot]==1 && count(hot)==0,"measured peak and no leak");
    std::cout<<"Runtime capacity "<<header.slots_per_shard<<": chunks, preflight, peaks and reuse OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
