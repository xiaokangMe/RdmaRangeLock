#include "rdma/rdma_transport.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <sys/resource.h>
using namespace rrl;
int main() {
    const auto* hostenv=std::getenv("RRL_TEST_RDMA_HOST");
    const auto* portenv=std::getenv("RRL_TEST_TXN_STATE_PORT");
    if(!portenv)return 77;
    rlimit original{};
    if(getrlimit(RLIMIT_NOFILE,&original))return 1;
    try {
        // 64 TCP sockets plus one shared device fit 128 descriptors. Opening
        // a new verbs device for every QP does not. No environment limit bump.
        rlimit limited=original;limited.rlim_cur=std::min<rlim_t>(128,original.rlim_cur);
        if(limited.rlim_cur<96 || setrlimit(RLIMIT_NOFILE,&limited))throw std::runtime_error("FD test setup");
        std::vector<std::unique_ptr<RdmaConnection>> connections;
        for(int i=0;i<64;++i) {
            auto c=std::make_unique<RdmaConnection>();
            if(!c->connect_client(hostenv?hostenv:"127.0.0.1",std::stoi(portenv)))
                throw std::runtime_error("64 QPs under FD budget failed");
            connections.push_back(std::move(c));
        }
        auto survivor=std::move(connections.front());
        connections.clear();
        std::uint64_t value=0;
        const auto m=survivor->remote_memory();
        if(!survivor->read(m.address,m.rkey,&value,8) || !value)
            throw std::runtime_error("shared PD destroyed before last QP");
        survivor.reset();
        // Once all references are gone, a new context can open and operate.
        RdmaConnection replacement;
        if(!replacement.connect_client(hostenv?hostenv:"127.0.0.1",std::stoi(portenv)))
            throw std::runtime_error("device reopen failed");
        const auto n=replacement.remote_memory();
        if(!replacement.read(n.address,n.rkey,&value,8))throw std::runtime_error("reopened device read");
        setrlimit(RLIMIT_NOFILE,&original);
        std::cout<<"64 independent QP/CQ/buffers under 128 FDs; shared device/PD lifetime OK\n";
    }catch(const std::exception& e) {
        setrlimit(RLIMIT_NOFILE,&original);std::cerr<<e.what()<<'\n';return 1;
    }
}
