#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "common/range.h"
#include "common/types.h"

namespace rrl {

enum class LatchMode : std::uint8_t {
    Shared,
    Exclusive,
};

struct KeyValue {
    idx_key_t key{0};
    void* value{nullptr};
};

// Stable index contract shared by v1 and v2. The implementation owns the
// page layout; transaction locks never reach into it.
class BTreeIndex {
public:
    virtual ~BTreeIndex() = default;
    virtual RC init(std::size_t order) = 0;
    virtual RC insert(idx_key_t key, void* value) = 0;
    virtual RC erase(idx_key_t key) = 0;
    virtual bool lookup(idx_key_t key, void*& value) const = 0;
    virtual std::vector<KeyValue> scan_range(const KeyRangeEnt& range) const = 0;
    virtual std::size_t size() const = 0;
};

}  // namespace rrl