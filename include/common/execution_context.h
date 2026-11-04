#pragma once
#include <cstdint>
namespace rrl {
bool coroutine_active() noexcept;
void cooperative_yield();
std::uint64_t execution_context_id() noexcept;
// Context-local attachment, distinct for every coroutine on a worker.
const void*& execution_index_owner() noexcept;
void*& execution_index_session() noexcept;
} // namespace rrl
