#include "txn/workload.h"
#include "txn/policy_workload.h"
#include "lock/native_policy.h"
#include "lock/lock_manager.h"
#include "index/btree.h"

#include <arpa/inet.h>
#include <endian.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace rrl;
namespace {
constexpr std::uint32_t kMagic = 0x52524c31U;
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kMaxOperations = 256;
volatile std::sig_atomic_t stopping = 0;
void stop_handler(int) { stopping = 1; }

#pragma pack(push, 1)
struct RequestHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t operations;
    std::uint64_t transaction;
};
struct WireOperation {
    std::uint8_t kind;
    std::uint8_t reserved[7];
    std::uint64_t key;
    std::uint64_t end_key;
    std::uint64_t value;
};
struct Response {
    std::uint32_t magic;
    std::uint32_t status;
    std::uint64_t committed;
    std::uint64_t aborted;
    std::uint64_t operations;
    std::uint64_t lock_acquires;
    std::uint64_t lock_conflicts;
    std::uint64_t checksum;
};
#pragma pack(pop)

std::uint64_t to_network64(std::uint64_t value) { return htobe64(value); }
std::uint64_t from_network64(std::uint64_t value) { return be64toh(value); }

bool transfer(int fd, void* data, std::size_t size, bool sending) {
    auto* bytes = static_cast<unsigned char*>(data);
    while (size != 0) {
        const auto n = sending ? ::send(fd, bytes, size, MSG_NOSIGNAL)
                               : ::recv(fd, bytes, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

std::map<std::string, std::string> parse_options(int argc, char** argv) {
    std::map<std::string, std::string> result;
    for (int i = 1; i < argc; ++i) {
        const std::string key(argv[i]);
        if (key.rfind("--", 0) != 0 || i + 1 >= argc)
            throw std::invalid_argument("options must be --name value");
        if (!result.emplace(key.substr(2), argv[++i]).second)
            throw std::invalid_argument("duplicate option: " + key);
    }
    return result;
}
std::string option(const std::map<std::string, std::string>& opts, const std::string& name,
                   const std::string& fallback = {}) {
    const auto it = opts.find(name);
    if (it != opts.end()) return it->second;
    if (!fallback.empty()) return fallback;
    throw std::invalid_argument("missing --" + name);
}
std::size_t integer_option(const std::map<std::string, std::string>& opts,
                           const std::string& name, std::size_t fallback) {
    const auto it = opts.find(name);
    if (it == opts.end()) return fallback;
    std::size_t used = 0;
    const auto value = std::stoull(it->second, &used);
    if (used != it->second.size()) throw std::invalid_argument("invalid --" + name);
    return static_cast<std::size_t>(value);
}
double real_option(const std::map<std::string, std::string>& opts,
                   const std::string& name, double fallback) {
    const auto it = opts.find(name);
    if (it == opts.end()) return fallback;
    std::size_t used = 0;
    const auto value = std::stod(it->second, &used);
    if (used != it->second.size()) throw std::invalid_argument("invalid --" + name);
    return value;
}
PolicyProtocol parse_protocol(const std::string& value) {
    if (value == "none") return PolicyProtocol::None;
    if (value == "record-2pl") return PolicyProtocol::Record;
    if (value == "gap-lock") return PolicyProtocol::Gap;
    if (value == "next-key") return PolicyProtocol::NextKey;
    if (value == "prior-key") return PolicyProtocol::PriorKey;
    if (value == "hierarchical") return PolicyProtocol::Hierarchy;
    if (value == "exact-range") return PolicyProtocol::ExactRange;
    if (value == "precision-lock") return PolicyProtocol::Precision;
    if (value == "griffin-pl") return PolicyProtocol::Griffin;
    throw std::invalid_argument("unsupported RPC protocol: " + value);
}
WorkloadConfig make_workload_config(const std::map<std::string, std::string>& opts) {
    WorkloadConfig config;
    config.kind = WorkloadTrace::parse_kind(option(opts, "workload"));
    config.key_count = integer_option(opts, "keys", 2097152);
    config.transaction_count = integer_option(opts, "transactions", 5000);
    config.operations_per_transaction = integer_option(opts, "ops", 10);
    config.range_length = integer_option(opts, "range", 32);
    config.threads = integer_option(opts, "threads", 32);
    config.hierarchy_bucket_width = integer_option(opts, "bucket", 64);
    config.tpcc_items_per_order = integer_option(opts, "items", 5);
    config.zipf_theta = real_option(opts, "zipf", 0.99);
    config.request_distribution = option(opts, "distribution", "auto");
    config.hotspot_data_fraction = real_option(opts, "hotspot-data", 0.2);
    config.hotspot_opn_fraction = real_option(opts, "hotspot-ops", 0.8);
    config.ycsb_unique_keys = integer_option(opts, "unique", 1) != 0;
    config.seed = integer_option(opts, "seed", 8);
    return config;
}
PolicyRunConfig make_policy_config(const std::map<std::string, std::string>& opts) {
    PolicyRunConfig config;
    config.protocol = parse_protocol(option(opts, "protocol"));
    config.wait_policy = parse_wait_policy(option(opts, "wait-policy", "no-wait"));
    config.max_attempts = integer_option(opts, "max-attempts", 1000);
    config.wait_timeout_ms = integer_option(opts, "wait-timeout-ms", 5000);
    config.lock_budget = integer_option(opts, "lock-budget", 16384);
    config.use_coroutines = false;
    config.coroutines = 1;
    return config;
}
std::uint64_t percentile(std::vector<std::uint64_t>& values, double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const auto position = std::min(values.size() - 1,
        static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1)));
    return values[position];
}

struct ServerState {
    WorkloadConfig config;
    PolicyRunConfig policy;
    BTreeLatch index;
    LockManager locks;
    LocalNativePolicyState transaction_state;
    std::unordered_map<idx_key_t, std::uint64_t> values;
    WorkloadTrace initial_trace;
    std::unique_ptr<std::atomic<std::uint8_t>[]> seen;
    std::mutex retained_mutex;
    std::vector<std::unique_ptr<WorkloadTrace>> retained;

    explicit ServerState(const std::map<std::string, std::string>& opts)
        : config(make_workload_config(opts)),
          policy(make_policy_config(opts)),
          transaction_state(std::max<std::size_t>(config.transaction_count, 1)),
          initial_trace(WorkloadTrace::generate(config)),
          seen(new std::atomic<std::uint8_t>[std::max<std::size_t>(config.transaction_count, 1)]) {
        if (!config.transaction_count || config.transaction_count > 1048576)
            throw std::invalid_argument("transactions must be in 1..1048576");
        if (index.init(integer_option(opts, "order", 128)) != RCOK)
            throw std::runtime_error("BTreeLatch initialization failed");
        if (!seed_index(index, initial_trace, values))
            throw std::runtime_error("server index preload failed");
        policy.shared_local_state = &transaction_state;
        policy.index_preloaded = true;
        for (std::size_t i = 0; i < config.transaction_count; ++i)
            seen[i].store(0, std::memory_order_relaxed);
    }

    void serve(int fd) {
        for (;;) {
            RequestHeader header{};
            if (!transfer(fd, &header, sizeof(header), false)) break;
            const auto count = ntohs(header.operations);
            const auto transaction = from_network64(header.transaction);
            Response response{};
            response.magic = htonl(kMagic);
            bool valid = ntohl(header.magic) == kMagic &&
                         ntohs(header.version) == kVersion &&
                         count != 0 && count <= kMaxOperations &&
                         transaction < config.transaction_count;
            std::vector<Operation> operations;
            operations.reserve(count);
            for (std::size_t i = 0; valid && i < count; ++i) {
                WireOperation wire{};
                if (!transfer(fd, &wire, sizeof(wire), false)) { ::close(fd); return; }
                if (wire.kind > static_cast<std::uint8_t>(OperationKind::RangeRead)) {
                    valid = false;
                    break;
                }
                operations.push_back({static_cast<OperationKind>(wire.kind),
                                      from_network64(wire.key),
                                      from_network64(wire.end_key),
                                      from_network64(wire.value)});
            }
            if (!valid) {
                response.status = 1;
                if (!transfer(fd, &response, sizeof(response), true)) break;
                continue;
            }
            std::uint8_t expected = 0;
            if (!seen[transaction].compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
                response.status = 1;
                if (!transfer(fd, &response, sizeof(response), true)) break;
                continue;
            }
            TransactionTrace one;
            one.txn_id = transaction + 1;
            one.operations = std::move(operations);
            auto trace = std::make_unique<WorkloadTrace>(
                WorkloadTrace::from_transactions(config, {std::move(one)}));
            const auto stats = run_policy_workload(index, locks, *trace, policy);
            if (stats.failed()) response.status = 2;
            else {
                response.committed = to_network64(stats.committed);
                response.aborted = to_network64(stats.aborted);
                response.operations = to_network64(stats.operations);
                response.lock_acquires = to_network64(stats.lock_acquires);
                response.lock_conflicts = to_network64(stats.lock_conflicts);
                response.checksum = to_network64(stats.checksum);
            }
            {
                std::lock_guard<std::mutex> guard(retained_mutex);
                retained.push_back(std::move(trace));
            }
            if (!transfer(fd, &response, sizeof(response), true)) break;
        }
        ::close(fd);
    }
};

int run_server(const std::map<std::string, std::string>& opts) {
    ServerState state(opts);
    const auto port = integer_option(opts, "port", 9622);
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) throw std::runtime_error("socket failed");
    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        ::listen(listener, 256) < 0)
        throw std::runtime_error("RPC bind/listen failed");
    std::signal(SIGTERM, stop_handler);
    std::signal(SIGINT, stop_handler);
    std::cout << "rpc_server_ready port=" << port << " keys=" << state.config.key_count
              << " transactions=" << state.config.transaction_count << std::endl;
    while (!stopping) {
        sockaddr_in peer{};
        socklen_t length = sizeof(peer);
        const int client = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &length);
        if (client < 0) {
            if (errno == EINTR) continue;
            if (stopping) break;
            throw std::runtime_error("accept failed");
        }
        std::thread([&state, client] { state.serve(client); }).detach();
    }
    ::close(listener);
    return 0;
}

int connect_to(const std::string& host, std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(fd);
        throw std::runtime_error("RPC connect failed");
    }
    return fd;
}

struct ClientResult {
    std::uint64_t committed{0}, aborted{0}, operations{0}, lock_acquires{0}, conflicts{0}, checksum{0};
};

int run_client(const std::map<std::string, std::string>& opts) {
    const auto config = make_workload_config(opts);
    const auto policy = make_policy_config(opts);
    const auto trace = WorkloadTrace::generate(config);
    const auto host = option(opts, "host");
    const auto port = static_cast<std::uint16_t>(integer_option(opts, "port", 9622));
    const auto warmup = integer_option(opts, "warmup-transactions", 0);
    if (warmup >= trace.transactions().size())
        throw std::invalid_argument("warmup-transactions must be less than transactions");
    const auto thread_count = std::max<std::size_t>(1, config.threads);
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex result_mutex;
    std::mutex latency_mutex;
    ClientResult result;
    std::vector<std::uint64_t> latency;

    auto run_phase = [&](std::size_t first, std::size_t last, bool measured) {
        next.store(first, std::memory_order_relaxed);
        std::vector<std::thread> workers;
        workers.reserve(thread_count);
        for (std::size_t worker = 0; worker < thread_count; ++worker) {
            workers.emplace_back([&] {
                int fd = -1;
                try {
                    fd = connect_to(host, port);
                    for (;;) {
                        const auto n = next.fetch_add(1, std::memory_order_relaxed);
                        if (n >= last || failed.load(std::memory_order_acquire)) break;
                        const auto& transaction = trace.transactions()[n];
                        RequestHeader header{htonl(kMagic), htons(kVersion),
                            htons(static_cast<std::uint16_t>(transaction.operations.size())),
                            to_network64(n)};
                        std::vector<WireOperation> wire(transaction.operations.size());
                        for (std::size_t i = 0; i < wire.size(); ++i) {
                            const auto& op = transaction.operations[i];
                            wire[i] = {static_cast<std::uint8_t>(op.kind), {},
                                to_network64(op.key), to_network64(op.end_key), to_network64(op.value)};
                        }
                        const auto start = std::chrono::steady_clock::now();
                        if (!transfer(fd, &header, sizeof(header), true) ||
                            !transfer(fd, wire.data(), wire.size() * sizeof(WireOperation), true))
                            throw std::runtime_error("RPC request failed");
                        Response response{};
                        if (!transfer(fd, &response, sizeof(response), false) ||
                            ntohl(response.magic) != kMagic || response.status)
                            throw std::runtime_error("RPC transaction rejected");
                        if (measured) {
                            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start).count();
                            std::lock_guard<std::mutex> result_guard(result_mutex);
                            result.committed += from_network64(response.committed);
                            result.aborted += from_network64(response.aborted);
                            result.operations += from_network64(response.operations);
                            result.lock_acquires += from_network64(response.lock_acquires);
                            result.conflicts += from_network64(response.lock_conflicts);
                            result.checksum += from_network64(response.checksum);
                            std::lock_guard<std::mutex> latency_guard(latency_mutex);
                            latency.push_back(static_cast<std::uint64_t>(elapsed));
                        }
                    }
                } catch (...) {
                    failed.store(true, std::memory_order_release);
                }
                if (fd >= 0) ::close(fd);
            });
        }
        for (auto& worker : workers) worker.join();
    };

    run_phase(0, warmup, false);
    if (failed.load()) throw std::runtime_error("RPC warm-up failed");
    const auto start = std::chrono::steady_clock::now();
    run_phase(warmup, trace.transactions().size(), true);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    if (failed.load()) throw std::runtime_error("RPC measured phase failed");
    const auto total = result.committed + result.aborted;
    std::cout << "workload,protocol,wait_policy,threads,keys,transactions,warmup_transactions,"
              << "committed,aborted,operations,lock_acquires,lock_conflicts,checksum,elapsed_ns,"
              << "throughput_txn_s,p50_rpc_ns,p99_rpc_ns\n";
    std::cout << WorkloadTrace::name(config.kind) << ',' << option(opts, "protocol") << ','
              << wait_policy_name(policy.wait_policy) << ',' << thread_count << ','
              << config.key_count << ',' << total << ',' << warmup << ',' << result.committed << ','
              << result.aborted << ',' << result.operations << ',' << result.lock_acquires << ','
              << result.conflicts << ',' << result.checksum << ',' << elapsed << ','
              << (elapsed ? static_cast<double>(total) * 1e9 / static_cast<double>(elapsed) : 0.0)
              << ',' << percentile(latency, 0.50) << ',' << percentile(latency, 0.99) << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto opts = parse_options(argc, argv);
        const auto role = option(opts, "role");
        if (role == "server") return run_server(opts);
        if (role == "client") return run_client(opts);
        throw std::invalid_argument("--role must be server or client");
    } catch (const std::exception& error) {
        std::cerr << "rpc_workload: " << error.what() << '\n';
        return 2;
    }
}
