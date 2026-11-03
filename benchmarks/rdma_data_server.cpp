#include "txn/shared_dataset.h"
#include "rdma/rdma_transport.h"
#include "common/config_parse.h"
#include <iostream>
#include <thread>

int main(int argc,char** argv) {
    try {
        std::string config;std::uint16_t port=19878,override_port=0;
        if((argc-1)%2)throw std::invalid_argument("data_server --config file [--port port]");
        for(int i=1;i<argc;i+=2) {
            const std::string k=argv[i];
            if(k=="--config" && config.empty())config=argv[i+1];
            else if(k=="--port" && !override_port) {
                auto n=rrl::parse_unsigned(argv[i+1],k);if(!n || n>65535)throw std::invalid_argument("port bounds");
                override_port=static_cast<std::uint16_t>(n);
            } else throw std::invalid_argument("unknown/duplicate data server argument");
        }
        if(config.empty())throw std::invalid_argument("--config required");
        rrl::WorkloadConfig workload;rrl::RdmaTreeConfig tree;
        rrl::load_shared_dataset_config(config,workload,tree,port);
        if(override_port)port=override_port;
        auto seed=rrl::make_shared_seed(rrl::WorkloadTrace::generate(workload));
        rrl::RdmaTreeMemory memory(tree,seed);
        rrl::RdmaListener listener(port);
        if(!listener.valid())throw std::runtime_error("data listener failed");
        std::cout<<"RDMA shared data server listening on port "<<port<<",seed_records="<<seed.size()
                 <<",bytes="<<memory.bytes()<<",seed_fingerprint="<<rrl::tree_seed_fingerprint(seed)<<std::endl;
        for(;;) {
            auto connection=std::make_unique<rrl::RdmaConnection>();
            if(!connection->accept_server(listener,memory.data(),memory.bytes())) {
                if(connection->rejected_handshake())continue;
                throw std::runtime_error("data server accept failed");
            }
            // No CPU tree access or unsafe guessed cleanup after a client
            // disappears: an uncertain data latch remains closed (fail-stop).
            std::thread([c=std::move(connection)]() mutable {c->wait_for_disconnect();}).detach();
        }
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 2;}
}
