#pragma once
#include "common/execution_context.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

namespace rrl::detail {
// Context-local pacing after a failed attempt has released ALL locks. This is
// retry admission, not a different lock wait policy. It consumes no additional
// attempts, changes no transaction age and has no shared random/cache line.
class RetryBackoff {
public:
    using Clock=std::chrono::steady_clock;
    explicit RetryBackoff(std::uint64_t context,bool enabled=true,unsigned spin_attempts=2)
        : random_(0x9e3779b97f4a7c15ULL*(context+1)),enabled_(enabled),spin_attempts_(spin_attempts) {}
    std::uint64_t pause(std::uint64_t failures,Clock::time_point deadline) {
        if(!enabled_ || !failures)return 0;
        const auto begin=Clock::now();if(begin>=deadline)return 0;
        random_^=random_<<13;random_^=random_>>7;random_^=random_<<17;
        if(failures<=spin_attempts_) {
            // A short desynchronized pause handles transient collisions without
            // a kernel scheduling round trip in the lightly contended case.
            const auto base=32U<<static_cast<unsigned>(std::min<std::uint64_t>(failures-1,5));
            const auto spins=base+static_cast<unsigned>(random_%base);
            if(coroutine_active())cooperative_yield();
            else for(unsigned n=0;n<spins;++n) {
#if defined(__i386__) || defined(__x86_64__)
                __builtin_ia32_pause();
#else
                std::this_thread::yield();
#endif
            }
        } else {
            // Repeated failures need a quiet interval for the winner to commit.
            // Full jitter prevents aborting workers from waking as a herd.
            const auto shift=static_cast<unsigned>(std::min<std::uint64_t>(failures-spin_attempts_-1,10));
            const auto ceiling=std::uint64_t{1}<<shift;
            const auto delay=std::chrono::microseconds(1+random_%ceiling);
            const auto until=std::min(deadline,begin+delay);
            if(coroutine_active()) {
                // Never sleep the OS worker while another coroutine can run.
                while(Clock::now()<until)cooperative_yield();
            } else std::this_thread::sleep_until(until);
        }
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count());
    }
private:
    std::uint64_t random_;
    bool enabled_;
    unsigned spin_attempts_;
};
} // namespace rrl::detail
