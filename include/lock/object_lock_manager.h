#pragma once
#include "lock/lock_manager.h"
#include <memory>
#include <string>
namespace rrl {
// Deneva-style per-object metadata CAS and holder identities. Separate from
// index pages: stable IDs cross the boundary; lock code never reads a page.
class ObjectLockManager final : public IRangeLockManager {
public:
    ObjectLockManager();
    ~ObjectLockManager() override;
    bool init_local(const std::vector<idx_key_t>& records,std::size_t nodes,std::size_t contexts,std::size_t budget);
    bool connect(const std::string&,std::uint16_t,const std::vector<idx_key_t>& records,std::size_t contexts);
    RC lock(txnid_t,LockMode,const KeyRangeEnt&,LockKind=LockKind::Predicate,std::vector<txnid_t>* = nullptr) override;
    RC unlock(txnid_t,const KeyRangeEnt&,LockKind=LockKind::Predicate) override;
    RC unlock_all(txnid_t) override;
    std::size_t get_lock_count(txnid_t=0) const override;
    std::uint64_t rdma_operations() const;
    std::uint64_t rdma_timeouts() const;
    std::uint64_t rdma_completion_errors() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Passive service; selected explicitly by the common lock-server entry point.
int run_object_lock_server(std::uint16_t,const std::vector<idx_key_t>& records,
                           std::size_t nodes,std::size_t sessions,std::size_t budget);
} // namespace rrl
