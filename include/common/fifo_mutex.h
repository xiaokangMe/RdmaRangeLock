#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace rrl {
// FIFO admission for local transport resources, not a transaction lock policy.
// No polling, delay or retry. A finite set of earlier operations cannot be
// overtaken by newcomers while a transaction is trying to execute or release.
class FifoMutex {
public:
    void lock() {
        std::unique_lock<std::mutex> guard(mutex_);
        const auto ticket = next_++;
        ready_.wait(guard, [&] { return serving_ == ticket; });
    }
    bool try_lock() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (next_ != serving_) return false;
        ++next_;
        return true;
    }
    void unlock() {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            ++serving_;
        }
        ready_.notify_all();
    }
private:
    friend struct FifoMutexTestPeer;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::uint64_t next_{0}, serving_{0};
};
} // namespace rrl
