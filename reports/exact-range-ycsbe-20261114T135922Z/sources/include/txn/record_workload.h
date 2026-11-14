#pragma once
#include "txn/workload.h"
namespace rrl {
// Standalone record-only executor; shared trace/index/metrics, specialized locks.
RunStats run_compact_record_workload(BTreeIndex&, const WorkloadTrace&, const PolicyRunConfig&);
RunStats run_compact_exact_workload(BTreeIndex&, const WorkloadTrace&, const PolicyRunConfig&);
RunStats run_compact_key_workload(BTreeIndex&, const WorkloadTrace&, const PolicyRunConfig&);
RunStats run_compact_hierarchy_workload(BTreeIndex&, const WorkloadTrace&, const PolicyRunConfig&);
}
