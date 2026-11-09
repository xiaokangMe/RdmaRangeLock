#include "citron/segment_tree.h"
#include "citron/backend.h"
#include "rdma/rdma_transport.h"
#include "common/config_parse.h"
#include <iostream>
#include <thread>
#include <memory>
int main(int argc,char** argv) {
  try {
    if(argc!=3)throw std::invalid_argument("usage: rrl_citron_lock_server port tree-height");
    const auto port=rrl::parse_unsigned(argv[1],"port"),height=rrl::parse_unsigned(argv[2],"tree-height");
    if(!port || port>65535 || !height || height>10)throw std::invalid_argument("Citron server geometry bounds");
    citron::SegmentTree tree(static_cast<int>(height));
    std::vector<std::uint64_t> words((tree.total_nodes()+2)*citron::kNodeWords);
    rrl::RdmaListener listener(static_cast<std::uint16_t>(port));
    if(!listener.valid())throw std::runtime_error("Citron listener failed");
    std::cout<<"Citron ready height="<<height<<" units="<<tree.units()<<" bytes="<<words.size()*8<<std::endl;
    for(;;) {
      auto connection=std::make_unique<rrl::RdmaConnection>();
      if(!connection->accept_server(listener,words.data(),words.size()*8)) {
        if(connection->rejected_handshake())continue;
        throw std::runtime_error("Citron server handshake failed");
      }
      // All ownership decisions use RDMA on the passive tree, never a TCP RPC.
      std::thread([c=std::move(connection)]()mutable{c->wait_for_disconnect();}).detach();
    }
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
