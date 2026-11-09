#include "citron/backend.h"

namespace citron {

bool Backend::read_node_many(const std::vector<std::uint64_t>& indices,
                             std::vector<Node>& out) {
    out.resize(indices.size());
    for (std::size_t i = 0; i < indices.size(); ++i)
        if (!read_node(indices[i], out[i])) return false;
    return true;
}

bool Backend::read_node_range(std::uint64_t first_index, std::size_t count,
                              std::vector<Node>& out) {
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i)
        if (!read_node(first_index + i, out[i])) return false;
    return true;
}

bool Backend::faa_many(const std::vector<FaaOp>& ops) {
    for (const auto& op : ops) {
        std::uint64_t previous = 0;
        if (!faa(op.index, op.field, op.add, previous)) return false;
    }
    return true;
}

}  // namespace citron
