#pragma once
#include "txn/workload.h"

namespace rrl::detail {
// Register predicates BEFORE reading or deciding whether a unique key exists.
// No cached B+tree neighbor, candidate range, or predicate/predicate comparison.
template<class Acquire>
bool precision_point(const Operation& op,Acquire&& acquire) {
    const auto key=KeyRangeEnt::point(op.key);
    if(!acquire(LockMode::S,key,LockKind::PrecisionPredicate))return false;
    return op.kind==OperationKind::Read || acquire(LockMode::X,key,LockKind::PrecisionUpdate);
}
} // namespace rrl::detail
