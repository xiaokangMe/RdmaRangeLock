#pragma once
#include <cstdint>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>
namespace rrl::object_detail {
constexpr std::uint64_t magic=0x52524c4f424a3033ULL;
struct Header {
    std::uint64_t signature{magic},bytes{0},records{0},nodes{0},sessions{0},budget{0},allocated{0},fingerprint{0};
    std::uint64_t session_base{128},object_base{0},holder_base{0},poison{0},dag_width{0},dag_paths{0},reserved[2]{};
};
struct Object {std::uint64_t mutex{0},head{0};};
struct Holder {std::uint64_t txn{0},mode{0},next{0};};
struct Session {std::uint64_t active{0},count{0};};
inline std::uint64_t fingerprint(const std::vector<std::uint64_t>& keys) {
    std::uint64_t h=1469598103934665603ULL;
    for(auto k:keys)for(unsigned i=0;i<8;++i){h^=k&255;h*=1099511628211ULL;k>>=8;}return h;
}
inline Header layout(std::size_t records,std::size_t nodes,std::size_t sessions,std::size_t budget,std::uint64_t fp) {
    if(!records || nodes<3 || !sessions || sessions>4096 || !budget || budget>1048576)throw std::invalid_argument("object lock geometry");
    Header h;h.records=records;h.nodes=nodes;h.sessions=sessions;h.budget=budget;h.fingerprint=fp;
    const auto max=std::numeric_limits<std::size_t>::max();
    h.object_base=sizeof(Header)+sessions*sizeof(Session);
    if(records>max-nodes || records+nodes>(max-h.object_base)/sizeof(Object))throw std::invalid_argument("object directory overflow");
    h.holder_base=h.object_base+(records+nodes)*sizeof(Object);
    if(budget>(max-h.holder_base)/sessions/sizeof(Holder))throw std::invalid_argument("holder pool overflow");
    h.bytes=h.holder_base+sessions*budget*sizeof(Holder);return h;
}
static_assert(sizeof(Header)==128,"stable object header");
}
