#pragma once

#include <cstdint>
#include <vector>

#include "common/range.h"
#include "lock/lock_manager.h"

namespace rrl {

// Optional capability: atomically publish several ranges for one transaction.
// It is deliberately separate from IRangeLockManager so the six protocol
// interfaces and their signatures remain unchanged.
class IRangeLockBatch {
public:
    virtual ~IRangeLockBatch() = default;
    virtual RC lock_batch(txnid_t txn_id, LockMode mode,
                          const std::vector<KeyRangeEnt>& ranges, LockKind kind,
                          std::vector<txnid_t>* conflicts = nullptr) = 0;
};

}  // namespace rrl
