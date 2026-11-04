#pragma once
#include "common/types.h"
#include <cstddef>
namespace rrl { namespace native_detail {
constexpr std::uint64_t magic=0x52524c4e54584e31ULL;
constexpr std::uint64_t max_id=(1ULL<<47)-1;
enum State:std::uint64_t {Idle=0,Running=1,Wounded=2,Committing=3};
struct alignas(64) Header {
    std::uint64_t signature{magic},slots{0},allocated{0},age{0},poison{0},policy{0};
};
struct alignas(32) Slot {std::uint64_t control{0},age{0},reserved[2]{};};
static_assert(sizeof(Slot)==32 && offsetof(Slot,age)==8,"RDMA state wire layout");
inline std::uint64_t control(txnid_t id,State s){return (id<<3)|s;}
} }
