#pragma once
#include <cstddef>
namespace rrl {
// Optional pre-Begin session setup. Keeps QP creation outside transaction locks
// and measured attempts without changing IRangeLockManager's six-version API.
class ILockBackendLifecycle {
public:
    virtual ~ILockBackendLifecycle() = default;
    virtual void prepare_lock_thread(std::size_t worker) = 0;
};
}
