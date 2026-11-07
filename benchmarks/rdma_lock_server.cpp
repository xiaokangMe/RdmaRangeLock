#include "lock/remote_lock_manager.h"
#include "common/config_parse.h"
#include "lock/object_lock_manager.h"
#include "txn/shared_dataset.h"
#include "../src/txn/leaf_hierarchy.h"
#include "../src/txn/dag_hierarchy.h"

#include <chrono>
#include <fstream>
#include <condition_variable>
#include <mutex>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using namespace rrl;

namespace {
[[noreturn]] void recovery_failed(std::uint64_t owner, const char* reason) {
    std::cerr << "RDMA recovery failed closed: owner=" << owner << " " << reason << std::endl;
    std::_Exit(2);
}

class RecoveryBudget {
public:
    RecoveryBudget(std::uint64_t owner, std::chrono::milliseconds duration)
        : deadline(std::chrono::steady_clock::now() + duration), watchdog_([this, owner] {
            std::unique_lock<std::mutex> guard(mutex_);
            if (!cv_.wait_until(guard, deadline, [this] { return done_; }))
                recovery_failed(owner, "overall recovery deadline");
        }) {}
    ~RecoveryBudget() {
        { std::lock_guard<std::mutex> guard(mutex_); done_ = true; }
        cv_.notify_one(); watchdog_.join();
    }
    const std::chrono::steady_clock::time_point deadline;
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_{false};
    std::thread watchdog_;
};

std::chrono::milliseconds recovery_timeout() {
    const char* value = std::getenv("RRL_RDMA_RECOVERY_TIMEOUT_MS");
    const auto ms = value ? parse_unsigned(value, "RRL_RDMA_RECOVERY_TIMEOUT_MS") : 5000;
    if (!ms || ms > 600000) throw std::invalid_argument("recovery timeout must be 1..600000 ms");
    return std::chrono::milliseconds(ms);
}

void clear_owner_state(std::uint16_t port, std::uint64_t owner, RdmaOpTag failed_tag, bool native_fail_stop, RemoteLockHeader layout) {
    if (owner == 0) return; // Unowned probes/recovery connections never publish locks.
    // Use NIC atomics for both ordinary clients and recovery. CPU atomics are
    // not a portable mutual exclusion mechanism against remote NIC atomics.
    RecoveryBudget budget(owner, recovery_timeout());
    RdmaConnection recovery;
    recovery.set_deadline(budget.deadline);
    if (!recovery.connect_client("127.0.0.1", port)) recovery_failed(owner, "connect");
    const auto remote = recovery.remote_memory();
    const auto token = owner | (std::uint64_t{1} << 63);
    std::vector<RemoteLockSlot> slots(layout.slots_per_shard);
    for (std::size_t shard = 0; shard < kRemoteLockShards; ++shard) {
        const auto base = remote.address + offsetof(RemoteLockRegion, shards) +
                          shard * layout.shard_stride;
        const auto gate = base + offsetof(RemoteLockShard, gate);
        const auto deadline = budget.deadline;
        for (;;) {
            std::uint64_t previous = 0;
            if (!recovery.compare_swap(gate, remote.rkey, 0, token, previous))
                recovery_failed(owner, "acquire CAS");
            if (previous == 0) break;
            if (previous == owner) {
                if (native_fail_stop)
                    recovery_failed(owner, "native owner disconnected with held gate; locks remain fenced");
                // A torn slot WRITE is not a recoverable transaction. Never
                // reopen its shard without a journal/publication protocol.
                if (failed_tag == RdmaOpTag::None || failed_tag == RdmaOpTag::StoreSlot)
                    recovery_failed(owner, "uncertain mutation while holding gate");
                if (!recovery.compare_swap(gate, remote.rkey, owner, token, previous) || previous != owner)
                    recovery_failed(owner, "takeover CAS");
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                recovery_failed(owner, "gate deadline");
            std::this_thread::yield();
        }
        std::uint64_t count = 0;
        if (!recovery.read(base + offsetof(RemoteLockShard, active_slots), remote.rkey,
                           &count, sizeof(count)) || count > layout.slots_per_shard)
            recovery_failed(owner, "count");
        for(std::size_t offset=0;offset<count*sizeof(RemoteLockSlot);) {
            const auto bytes=std::min<std::size_t>(1u<<20,count*sizeof(RemoteLockSlot)-offset);
            if(!recovery.read(base+offsetof(RemoteLockShard,slots)+offset,remote.rkey,
                reinterpret_cast<char*>(slots.data())+offset,bytes))recovery_failed(owner,"slots");
            offset+=bytes;
        }
        if (native_fail_stop) {
            for (std::uint64_t i = 0; i < count; ++i)
                if (slots[i].owner == owner)
                    recovery_failed(owner, "native owner disconnected with published locks; refusing early unlock");
        }
        std::uint64_t kept = 0;
        for (std::uint64_t i = 0; i < count; ++i)
            if (slots[i].owner != owner) slots[kept++] = slots[i];
        if (kept != count) {
            for(std::size_t offset=0;offset<kept*sizeof(RemoteLockSlot);) {
                const auto bytes=std::min<std::size_t>(1u<<20,kept*sizeof(RemoteLockSlot)-offset);
                if(!recovery.write(base+offsetof(RemoteLockShard,slots)+offset,remote.rkey,
                    reinterpret_cast<char*>(slots.data())+offset,bytes))recovery_failed(owner,"compact");
                offset+=bytes;
            }
            if (!recovery.write(base + offsetof(RemoteLockShard, active_slots), remote.rkey,
                                &kept, sizeof(kept))) recovery_failed(owner, "publish count");
        }
        std::uint64_t previous = 0;
        if (!recovery.compare_swap(gate, remote.rkey, token, 0, previous) || previous != token)
            recovery_failed(owner, "release CAS");
    }
}
}

int run(int argc, char** argv) {
    if(argc==4 && std::string(argv[2])=="--objects") {
        const auto port=parse_unsigned(argv[1],"port");if(!port || port>65535)throw std::invalid_argument("port bounds");
        WorkloadConfig c;RdmaTreeConfig t;std::uint16_t ignored;
        load_shared_dataset_config(argv[3],c,t,ignored);
        const auto trace=WorkloadTrace::generate(c);
        // Runtime geometry is ignored by the dataset parser; read it explicitly.
        std::ifstream input(argv[3]);const std::string text((std::istreambuf_iterator<char>(input)),{});
        const auto entries=FlatConfigParser(text).parse();
        std::size_t contexts=64,budget=16384,threads=1,coroutines=1,paths=2;
        bool dag=false,bucket_seen=false;
        for(const auto& e:entries)if(e.first=="hierarchy_layout")dag=e.second.text=="dag";
        for(const auto& e:entries) {
            if(e.first=="hierarchy_index_paths")paths=parse_unsigned(e.second.text,e.first);
            else if(dag && (e.first=="hierarchy_bucket_width" || e.first=="bucket")) {
                if(bucket_seen || e.second.kind!=ConfigScalar::Number)throw std::invalid_argument("DAG bucket width must be a unique numeric field");
                bucket_seen=true;c.hierarchy_bucket_width=parse_unsigned(e.second.text,e.first);
            }
        }
        for(const auto& e:entries)if(e.first=="threads")threads=parse_unsigned(e.second.text,e.first);
            else if(e.first=="coroutines")coroutines=parse_unsigned(e.second.text,e.first);
            else if(e.first=="lock_budget")budget=parse_unsigned(e.second.text,e.first);
        if(!threads || !coroutines || threads>4096/coroutines)throw std::invalid_argument("object context bounds");
        contexts=std::max(contexts,threads*coroutines+16);
        const auto nodes=dag?detail::dag_node_capacity(trace,c.hierarchy_bucket_width,paths):t.page_capacity+2;
        return run_object_lock_server(static_cast<std::uint16_t>(port),detail::object_record_keys(trace),nodes,contexts,budget,
                                      dag?c.hierarchy_bucket_width:0,dag?paths:0);
    }
    bool native_fail_stop=false;
    std::size_t capacity=kRemoteLockSlotsPerShard;
    const auto parsed = argc > 1 ? parse_unsigned(argv[1], "port") : 19876;
    if (!parsed || parsed > 65535) throw std::invalid_argument("port must be 1..65535");
    const auto port=static_cast<std::uint16_t>(parsed);
    for(int i=2;i<argc;++i) {
        const std::string flag=argv[i];
        if(flag=="--native-fail-stop" && !native_fail_stop)native_fail_stop=true;
        else if(flag=="--slots-per-shard" && i+1<argc)capacity=parse_unsigned(argv[++i],"slots-per-shard");
        else throw std::invalid_argument("usage: lock_server [port] [--native-fail-stop] [--slots-per-shard N]");
    }
    const auto layout=remote_lock_layout(capacity);
    (void)recovery_timeout();
    if (const char* value = std::getenv("RRL_RDMA_CONTROL_TIMEOUT_MS")) {
        const auto ms = parse_unsigned(value, "RRL_RDMA_CONTROL_TIMEOUT_MS");
        if (!ms || ms > 600000) throw std::invalid_argument("control timeout must be 1..600000 ms");
    }
    std::vector<std::uint64_t> region(layout.bytes/sizeof(std::uint64_t),0);
    std::memcpy(region.data(),&layout,sizeof(layout));
    RdmaListener listener(port);
    if (!listener.valid()) { std::cerr << "RDMA control listen failed\n"; return 2; }
    std::cout << "RDMA lock server listening on port " << port << ",slots_per_shard=" << capacity << ",bytes=" << layout.bytes << std::endl;
    for (;;) {
        auto* connection = new RdmaConnection();
        if (!connection->accept_server(listener, region.data(), layout.bytes)) {
            const bool rejected = connection->rejected_handshake();
            delete connection;
            if (rejected) { std::cerr << "RDMA control handshake rejected\n"; continue; }
            std::cerr << "RDMA lock server accept failed\n";
            return 2;
        }
        std::thread([connection, port, native_fail_stop, layout] {
            connection->wait_for_disconnect();
            const auto owner = connection->peer_owner_token();
            const auto tag = connection->peer_failure_tag();
            // Destroy the server QP and invalidate its MR before any recovery
            // can acquire gates or read slots written through that connection.
            delete connection;
            clear_owner_state(port, owner, tag, native_fail_stop, layout);
        }).detach();
    }
}

int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 2; }
}
