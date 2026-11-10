#include "index/rolex_rdma.h"
#include "txn/shared_dataset.h"
#include "common/config_parse.h"
#include "rdma/rdma_transport.h"
#include <iostream>
#include <thread>
#include <stdexcept>
#include <algorithm>
int main(int argc,char**argv){try{std::string config;std::uint16_t port=19879,override_port=0;if((argc-1)%2)throw std::invalid_argument("rolex server args");for(int i=1;i<argc;i+=2){std::string k=argv[i];if(k=="--config"&&config.empty())config=argv[i+1];else if(k=="--port"&&!override_port){auto n=rrl::parse_unsigned(argv[i+1],k);if(!n||n>65535)throw std::invalid_argument("port");override_port=(std::uint16_t)n;}else throw std::invalid_argument("unknown argument");}if(config.empty())throw std::invalid_argument("--config required");rrl::WorkloadConfig w;rrl::RdmaTreeConfig t;rrl::load_shared_dataset_config(config,w,t,port);if(override_port)port=override_port;auto seed=rrl::make_shared_seed(rrl::WorkloadTrace::generate(w));rolex::remote::RemoteRolexConfig c;c.leaf_capacity=std::max<std::size_t>(4096,(seed.size()+63)/64*2+1024);rolex::remote::RemoteRolexMemory mem(c,seed);rrl::RdmaListener listener(port);if(!listener.valid())throw std::runtime_error("ROLEX listener failed");std::cout<<"remote ROLEX server listening port="<<port<<",seed_records="<<seed.size()<<",leaf_capacity="<<c.leaf_capacity<<",bytes="<<mem.bytes()<<",seed_fingerprint="<<rolex::remote::seed_fingerprint(seed)<<std::endl;for(;;){auto conn=std::make_unique<rrl::RdmaConnection>();if(!conn->accept_server(listener,mem.data(),mem.bytes())){if(conn->rejected_handshake())continue;throw std::runtime_error("ROLEX accept failed");}std::thread([c=std::move(conn)]()mutable{c->wait_for_disconnect();}).detach();}}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}
