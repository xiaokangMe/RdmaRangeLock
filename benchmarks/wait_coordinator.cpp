#include "lock/wait_policy.h"
#include "common/config_parse.h"
#include <iostream>
int main(int argc,char** argv) {
    try {
        if(argc<2 || argc>4)throw std::invalid_argument("usage: rrl_wait_coordinator port [lock-budget] [idle-timeout-ms]");
        const auto port=rrl::parse_unsigned(argv[1],"port");
        if(!port || port>65535)throw std::invalid_argument("invalid port");
        const auto budget=argc>2 ? rrl::parse_unsigned(argv[2],"lock-budget") : 16384;
        const auto idle=argc>3 ? rrl::parse_unsigned(argv[3],"idle-timeout-ms") : 10000;
        return rrl::run_wait_coordinator(static_cast<std::uint16_t>(port),budget,idle);
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
