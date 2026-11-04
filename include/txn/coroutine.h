#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
namespace rrl {
// Deneva-style stackful worker routines, using the system ucontext API.
// A scheduler runs on one OS worker, never migrates live stacks.
class CoroutineScheduler {
public:
    CoroutineScheduler(std::size_t routines, std::size_t stack_bytes);
    ~CoroutineScheduler();
    void run(const std::function<void(std::size_t)>&);
    std::uint64_t switches() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rrl
