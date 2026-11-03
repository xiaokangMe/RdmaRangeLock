#pragma once
#include "index/rdma_btree.h"
#include "txn/workload.h"

namespace rrl {
// Initialization only. Never called inside a measured transaction.
TreeSeed make_shared_seed(const WorkloadTrace&);
void load_shared_dataset_config(const std::string&,WorkloadConfig&,RdmaTreeConfig&,std::uint16_t&);
} // namespace rrl
