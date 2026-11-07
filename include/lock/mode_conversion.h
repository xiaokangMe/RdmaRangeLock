#pragma once
#include "common/range.h"
#include <vector>
namespace rrl {
// Optional bounded-Node capability. Replaces (does not join) an existing
// object's mode atomically with respect to all conflicting lock requests.
// Conflict/gate/capacity failures preserve the old mode. ERROR invalidates
// the run. The transaction layer must prove continuous protection before a
// downgrade: acquire the replacement logical protection before conversion.
// No IRangeLockManager signature changes or implicit early unlock.
class ILockModeConversion {
public:
    virtual ~ILockModeConversion()=default;
    virtual RC convert_lock(txnid_t,LockMode,const KeyRangeEnt&,LockKind,
                            std::vector<txnid_t>* conflicts=nullptr)=0;
};
}
