#include "lock/record_lock_table.h"
#include "txn/record_workload.h"
#include <future>
#include <iostream>
#include <thread>
#include <atomic>

using namespace rrl;
using Table=RecordLockTable;
constexpr std::uint64_t budget=3000000000ULL;
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class F> void until(F f,const char* message) {
    const auto deadline=Table::Clock::now()+std::chrono::seconds(3);
    while(!f()) {require(Table::Clock::now()<deadline,message);std::this_thread::yield();}
}
void layout_and_upgrade() {
    Table table(3,{100,UINT64_MAX,100},3);
    require(table.lock_bytes()==40 && table.locate(UINT64_MAX) && !table.locate(99),"stable dense/sparse resources");
    Table::Client a(table,0,2),b(table,1,2),c(table,2,1);
    const auto older=a.begin(0,budget);require(older.result==TxnResult::Ok,"begin");
    require(b.begin(0,budget).result==TxnResult::Ok,"begin b");
    auto& x=*table.locate(0);auto& y=*table.locate(1);
    require(a.acquire<WaitPolicy::NoWait>(x,LockMode::S).result==TxnResult::Ok,"first S");
    require(a.acquire<WaitPolicy::NoWait>(x,LockMode::S).result==TxnResult::Ok && a.held_count()==1,"deduplicate S");
    require(b.acquire<WaitPolicy::NoWait>(x,LockMode::S).result==TxnResult::Ok,"S compatible");
    require(a.acquire<WaitPolicy::NoWait>(x,LockMode::X).result==TxnResult::Conflict,"two readers cannot upgrade");
    b.finish();
    require(a.acquire<WaitPolicy::NoWait>(x,LockMode::X).result==TxnResult::Ok && a.held_count()==1,"sole reader upgrades");
    require(a.acquire<WaitPolicy::NoWait>(x,LockMode::S).result==TxnResult::Ok,"own X covers S");
    require(b.begin(0,budget).result==TxnResult::Ok,"reuse b");
    require(b.acquire<WaitPolicy::NoWait>(x,LockMode::S).result==TxnResult::Conflict,"X excludes S");
    require(b.acquire<WaitPolicy::NoWait>(x,LockMode::X).result==TxnResult::Conflict,"X excludes X");
    require(a.acquire<WaitPolicy::NoWait>(y,LockMode::X).result==TxnResult::Ok,"second resource");
    require(a.acquire<WaitPolicy::NoWait>(*table.locate(2),LockMode::S).result==TxnResult::Conflict && a.capacity_abort(),"budget before grant");
    a.finish();b.finish();
    require(table.empty(),"abort/finish releases all");
    const auto retry=a.begin(older.age,budget);
    require(retry.id!=older.id && retry.age==older.age,"attempt identity and logical age");a.finish();
    Table edge(1,{},64);Table::Client last(edge,63,1),reader(edge,0,1);last.begin(0,budget);reader.begin(0,budget);
    auto& edge_record=*edge.locate(0);
    require(last.acquire<WaitPolicy::NoWait>(edge_record,LockMode::S).result==TxnResult::Ok,"64th context shared lock");
    require(reader.acquire<WaitPolicy::NoWait>(edge_record,LockMode::S).result==TxnResult::Conflict,"64th context shared lock is conservatively exclusive");
    last.finish();require(reader.acquire<WaitPolicy::NoWait>(edge_record,LockMode::S).result==TxnResult::Ok,"reader proceeds after 64th context");
    reader.finish();require(edge.empty(),"last context releases lock");
}

template<WaitPolicy Policy>
void waiting_and_commit() {
    Table table(2,{},2);Table::Client old(table,0,2),young(table,1,2);
    old.begin(0,budget);young.begin(0,budget);
    auto& x=*table.locate(0);
    require(old.acquire<Policy>(x,LockMode::X).result==TxnResult::Ok,"old X");
    if constexpr(Policy==WaitPolicy::WaitDie) {
        require(young.acquire<Policy>(x,LockMode::S).result==TxnResult::Conflict,"younger wait-die abort");
        old.finish();young.finish();
    } else {
        std::atomic<bool> started{false};
        auto future=std::async(std::launch::async,[&]{started=true;return young.acquire<Policy>(x,LockMode::S);});
        until([&]{return started.load();},"young started");
        require(old.prepare_commit<Policy>()==TxnResult::Ok,"younger cannot wound older");
        old.finish();require(future.get().result==TxnResult::Ok,"young eventually proceeds");young.finish();
    }
    old.begin(0,budget);young.begin(0,budget);
    require(young.acquire<Policy>(x,LockMode::S).result==TxnResult::Ok,"young S");
    auto waiting=std::async(std::launch::async,[&]{return old.acquire<Policy>(x,LockMode::X);});
    if constexpr(Policy==WaitPolicy::WoundWait) {
        until([&]{return young.check()==TxnResult::Wounded;},"older wounds S holder");
        require(young.prepare_commit<Policy>()==TxnResult::Wounded,"wound excludes commit");
    }
    young.finish();require(waiting.get().result==TxnResult::Ok,"old wakes after reader release");old.finish();
    // A committed permission cannot be stolen even by an older transaction.
    old.begin(0,budget);young.begin(0,budget);
    young.acquire<Policy>(x,LockMode::X);
    require(young.prepare_commit<Policy>()==TxnResult::Ok,"commit fence");
    auto fence=std::async(std::launch::async,[&]{return old.acquire<Policy>(x,LockMode::S);});
    require(young.check()==TxnResult::Ok,"committing cannot be wounded");
    young.finish();require(fence.get().result==TxnResult::Ok,"commit finishes before waiter");old.finish();
    require(table.empty(),"waiting leaves no locks");
}

template<WaitPolicy Policy>
void opposite_upgrades() {
    Table table(1,{},2);Table::Client old(table,0,1),young(table,1,1);
    old.begin(0,budget);young.begin(0,budget);auto& x=*table.locate(0);
    old.acquire<Policy>(x,LockMode::S);young.acquire<Policy>(x,LockMode::S);
    auto waiting=std::async(std::launch::async,[&]{return old.acquire<Policy>(x,LockMode::X);});
    if constexpr(Policy==WaitPolicy::WaitDie) {
        require(young.acquire<Policy>(x,LockMode::X).result==TxnResult::Conflict,"young upgrade dies");
    } else {
        until([&]{return young.check()==TxnResult::Wounded;},"upgrade wounds younger");
        require(young.acquire<Policy>(x,LockMode::X).result==TxnResult::Wounded,"wounded upgrade returns");
    }
    young.finish();require(waiting.get().result==TxnResult::Ok,"older upgrade completes");old.finish();
    require(table.empty(),"upgrade cleanup");
}

template<WaitPolicy Policy>
void parallel_transactions() {
    constexpr unsigned contexts=8,iterations=32;
    Table table(4,{},contexts);
    std::uint64_t values[4]={0,0,0,0};
    std::atomic<unsigned> ready{0};std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for(unsigned slot=0;slot<contexts;++slot)workers.emplace_back([&,slot]{
        try {
            Table::Client client(table,slot,4);++ready;
            until([&]{return ready==contexts;},"parallel start");
            for(unsigned i=0;i<iterations;++i) {
                std::uint64_t age=0;bool committed=false;
                const auto limit=Table::Clock::now()+std::chrono::seconds(10);
                while(!committed) {
                    require(Table::Clock::now()<limit,"parallel transaction progress");
                    const auto begin=client.begin(age,budget);require(begin.result==TxnResult::Ok,"parallel begin");age=begin.age;
                    const auto key=(i+slot)%4;
                    auto result=client.acquire<Policy>(*table.locate(key),LockMode::S).result;
                    if(result==TxnResult::Ok)result=client.acquire<Policy>(*table.locate(key),LockMode::X).result;
                    if(result==TxnResult::Ok)result=client.prepare_commit<Policy>();
                    if(result==TxnResult::Ok){++values[key];committed=true;}
                    client.finish();if(!committed)std::this_thread::yield();
                }
            }
        }catch(const std::exception& e){std::cerr<<e.what()<<'\n';failed=true;}
    });
    for(auto& worker:workers)worker.join();
    require(!failed,"parallel worker failed");
    require(values[0]+values[1]+values[2]+values[3]==contexts*iterations,"X excludes and publishes increments");
    require(table.empty(),"slot reuse leaves no lock bits");
}

void own_write_equivalence() {
    WorkloadConfig c;c.key_count=4;c.transaction_count=1;c.threads=1;c.operations_per_transaction=9;c.range_length=8;
    const std::vector<Operation> ops={
        {OperationKind::Update,1,0,91},{OperationKind::Read,1,0,0},
        {OperationKind::Insert,8,0,108},{OperationKind::RangeRead,0,12,0},
        {OperationKind::Delete,1,0,0},{OperationKind::Read,1,0,0},
        {OperationKind::Insert,1,0,191},{OperationKind::RangeRead,0,12,0},
        {OperationKind::Read,UINT64_MAX,0,0}};
    const auto trace=WorkloadTrace::from_transactions(c,{{1,ops}});
    for(auto policy:{WaitPolicy::NoWait,WaitPolicy::WaitDie,WaitPolicy::WoundWait}) {
        BTreeLatch generic,compact;generic.init(4);compact.init(4);LockManager locks;
        PolicyRunConfig cfg;cfg.protocol=PolicyProtocol::Record;cfg.use_coroutines=false;cfg.wait_policy=policy;
        const auto expected=run_policy_workload(generic,locks,trace,cfg);
        const auto actual=run_compact_record_workload(compact,trace,cfg);
        require(!expected.failed() && !actual.failed() && actual.committed==1,"own-write commit");
        require(actual.checksum==expected.checksum && actual.rows_found==expected.rows_found,"read-own-write and scan equivalence");
        require(generic.validate() && compact.validate() && generic.size()==compact.size(),"mutation structural invariants");
        for(idx_key_t key:{idx_key_t(0),idx_key_t(1),idx_key_t(8),UINT64_MAX}) {
            void *a=nullptr,*b=nullptr;const auto found=generic.lookup(key,a);
            require(found==compact.lookup(key,b),"membership matches");
            (void)a;(void)b; // Seed value addresses are intentionally run-local.
        }
    }
}
int main() {
    try {
        std::cerr<<"layout\n";layout_and_upgrade();
        std::cerr<<"wait-die\n";waiting_and_commit<WaitPolicy::WaitDie>();
        std::cerr<<"wound-wait\n";waiting_and_commit<WaitPolicy::WoundWait>();
        std::cerr<<"upgrades\n";opposite_upgrades<WaitPolicy::WaitDie>();opposite_upgrades<WaitPolicy::WoundWait>();
        std::cerr<<"stress no-wait\n";parallel_transactions<WaitPolicy::NoWait>();
        std::cerr<<"stress wait-die\n";parallel_transactions<WaitPolicy::WaitDie>();
        std::cerr<<"stress wound-wait\n";parallel_transactions<WaitPolicy::WoundWait>();
        std::cerr<<"executor\n";own_write_equivalence();
        std::cout<<"Record bitmap: S/X, upgrades, commit/wound, reuse, capacity, concurrent publication and executor equivalence OK\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
