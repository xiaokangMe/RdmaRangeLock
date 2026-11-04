#include "lock/native_policy.h"
#include "common/config_parse.h"
#include <iostream>
int main(int argc,char** argv) {
    try {
        if(argc<2 || argc>3)throw std::invalid_argument("usage: rrl_rdma_txn_server port [slots]");
        const auto port=rrl::parse_unsigned(argv[1],"port");
        const auto slots=argc==3?rrl::parse_unsigned(argv[2],"slots"):4096;
        if(!port || port>65535)throw std::invalid_argument("port bounds");
        return rrl::run_native_txn_server(static_cast<std::uint16_t>(port),slots);
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
