// FAST 2023 Citron Alg.1–3 adapter; transaction locks only, no index access.
#pragma once
#include "citron/client.h"
#include "lock/lock_manager.h"
#include "lock/backend_lifecycle.h"
#include <memory>
#include <string>
namespace citron {
struct LockManagerConfig {
    std::string server_host;
    std::uint16_t server_port{18515};
    int tree_height{4};
    std::size_t max_sessions{128}, lock_budget{16384};
    CitronConfig client;
    // Paper protocol is exclusive-only. Conservatively map S to X or reject.
    bool shared_as_exclusive{true};
    bool no_wait{true};
};
class CitronLockManager final : public rrl::IRangeLockManager, public rrl::ILockBackendLifecycle {
public:
    explicit CitronLockManager(LockManagerConfig);
    ~CitronLockManager() override;
    rrl::RC lock(rrl::txnid_t,rrl::LockMode,const rrl::KeyRangeEnt&,rrl::LockKind=rrl::LockKind::Predicate,std::vector<rrl::txnid_t>* =nullptr) override;
    rrl::RC unlock(rrl::txnid_t,const rrl::KeyRangeEnt&,rrl::LockKind=rrl::LockKind::Predicate) override;
    rrl::RC unlock_all(rrl::txnid_t) override;
    std::size_t get_lock_count(rrl::txnid_t=0) const override;
    void prepare_lock_thread(std::size_t) override;
    struct Diagnostics {
        std::uint64_t lock_calls{0},lock_grants{0},lock_aborts{0},lock_errors{0};
        std::uint64_t unlock_calls{0},unlock_failures{0},rdma_operations{0};
        std::uint64_t shared_mapped_to_exclusive{0},sessions{0};
    };
    Diagnostics diagnostics() const;
    void reset_diagnostics();
    const SegmentTree& tree() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
