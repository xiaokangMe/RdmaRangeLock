# RPC execution path

rrl_rpc_workload is the third execution path for this project. The client sends one complete transaction per TCP RPC request; the server owns the B+ tree, values, and lock table and executes the transaction.

- .66 runs the client and generates the same YCSB/TPC-C traces as the local runner.
- .67 runs the server and initializes BTreeLatch and LockManager. The client has no index or data copy.
- One request frame contains all operations of one transaction. The response carries commit/abort, operation, lock, and checksum counters.
- The client reports p50_rpc_ns, p99_rpc_ns, and transaction throughput. These latencies include the RPC round trip and server-side execution.
- This path uses ordinary TCP request/response and is separate from the RDMA one-sided path.
- Duplicate transaction numbers and malformed frames are rejected.

Build:
cmake --build build-shared --target rrl_rpc_workload -j4

Run one case on .66. The script copies the binary to .67, starts the server, waits for readiness, runs the client, and cleans up:
python3 scripts/run_rpc_case.py --workload ycsb-e --protocol exact-range --wait-policy no-wait --range 32 --threads 32 --transactions 5000

Use --dry-run first to inspect commands. A formal RPC matrix should be added only after this single-case path is stable; do not mix its CSV with local or RDMA one-sided results.
