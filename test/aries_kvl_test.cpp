#include "index/aries_kvl.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <random>
#include <map>
#include <stdexcept>
#include <thread>

using namespace rrl;
using namespace rrl::aries;
using namespace std::chrono_literals;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void ok(Status status){require(status==Status::Ok,"expected successful action");}
struct Signal {
    std::mutex mutex;std::condition_variable cv;bool ready=false;
    void set(){std::lock_guard<std::mutex> guard(mutex);ready=true;cv.notify_all();}
    void wait(){std::unique_lock<std::mutex> guard(mutex);cv.wait(guard,[&]{return ready;});}
};

void basic_and_undo() {
    for(bool unique:{false,true})for(std::size_t order:{4,5,16}) {
        Index index(order,unique);std::vector<Entry> initial;
        for(std::uint64_t n=0;n<128;++n)initial.push_back({n*10,n,n+7});
        index.load(initial);require(index.validate() && index.height()>1,"load/split tree");
        auto tx=index.begin();Entry row;
        require(index.fetch(*tx,55,Start::Equal,row)==Status::NotFound,"negative lookup");
        require(index.held_key(*tx,60)==LockMode::S,"negative lookup S-locks actual successor");
        ok(index.insert(*tx,{55,1000,9}));
        require(index.held_key(*tx,55)==LockMode::X,"Figure 7 replication after negative lookup");
        require(index.size()==129,"insert occurs before commit");
        ok(index.erase(*tx,70,7));require(index.size()==128,"delete occurs before commit");
        ok(index.abort(*tx));require(index.inspect()==initial && index.validate(),"logical undo restores entries through splits");
        require(index.metrics().live_lock_names==0,"abort releases key/record lock storage");
        auto duplicate=index.begin();auto duplicate_status=index.insert(*duplicate,{40,4,8});
        if(duplicate_status!=Status::Duplicate)std::cerr<<"unique="<<unique<<" order="<<order<<" duplicate status="<<int(duplicate_status)<<" size="<<index.size()<<"\n";
        require(duplicate_status==Status::Duplicate,"exact pair duplicate");
        if(unique)require(index.insert(*duplicate,{40,9999,8})==Status::Duplicate,"unique value duplicate");
        else ok(index.insert(*duplicate,{40,9999,8}));
        ok(index.abort(*duplicate));require(index.inspect()==initial,"duplicate rollback");
        // uint64 max is an ordinary value; EOF must be a distinct lock name.
        auto max=index.begin();ok(index.insert(*max,{UINT64_MAX,5000,2}));ok(index.commit(*max));
        auto eof=index.begin();require(index.fetch(*eof,UINT64_MAX,Start::Greater,row)==Status::NotFound,"EOF lookup");
        require(index.held_key(*eof,0,true)==LockMode::S && !index.held_key(*eof,UINT64_MAX),"EOF has distinct namespace");
        ok(index.commit(*eof));require(index.validate(),"max key tree valid");
    }
}

void duplicate_cursor() {
    Index index(4,false);std::vector<Entry> seed;
    for(std::uint64_t rid=1;rid<=32;++rid)seed.push_back({50,rid,rid});
    seed.push_back({60,100,100});index.load(seed);
    auto tx=index.begin();Index::Cursor cursor;Entry row;std::vector<Entry> rows;
    auto status=index.open(*tx,50,Start::Equal,50,Stop::Equal,cursor,row);
    while(status==Status::Ok){rows.push_back(row);status=index.next(*tx,cursor,row);}
    require(rows.size()==32 && rows.front().rid==1 && rows.back().rid==32,"RID cursor crosses duplicate leaves");
    require(!index.held_key(*tx,60),"5.2 equality scan does not lock next different value");
    require(index.held_key(*tx,50)==LockMode::S,"duplicate values share one key-value lock");
    ok(index.commit(*tx));
    // Inserting a duplicate needs no successor lock, even if successor is S locked.
    auto reader=index.begin();ok(index.fetch(*reader,60,Start::Equal,row));
    auto writer=index.begin();ok(index.insert(*writer,{50,200,3}));
    require(index.held_key(*writer,50)==LockMode::IX && !index.held_key(*writer,60),"duplicate Insert skips next key");
    ok(index.commit(*writer));ok(index.commit(*reader));
    auto scan=index.begin();ok(index.scan(*scan,50,60,rows));require(rows.size()==33,"half-open scan all duplicate RIDs");ok(index.commit(*scan));
    require(index.validate(),"duplicate tree valid");
}

void figure1_modes_and_eof() {
    // Figure 1: a unique new value is IX, unless next-key state replication
    // requires X. This distinguishes section 5 from the simple section 3 model.
    Index unique(16,true);unique.load({{10,1,1},{50,5,5},{90,9,9}});
    Entry row;auto plain=unique.begin();ok(unique.insert(*plain,{30,3,3}));
    require(unique.held_key(*plain,30)==LockMode::IX && !unique.held_key(*plain,50),"unique Insert IX and instant next-key duration");
    ok(unique.abort(*plain));
    auto ix=unique.begin();ok(unique.insert(*ix,{70,7,7}));ok(unique.insert(*ix,{60,6,6}));
    require(unique.held_key(*ix,60)==LockMode::IX,"IX successor does not cause X replication");
    ok(unique.fetch(*ix,70,Start::Equal,row));
    require(unique.held_key(*ix,70)==LockMode::SIX,"S join IX is SIX");
    ok(unique.insert(*ix,{65,65,65}));
    require(unique.held_key(*ix,65)==LockMode::X,"SIX successor replicates X");
    ok(unique.insert(*ix,{63,63,63}));
    require(unique.held_key(*ix,63)==LockMode::X,"X successor replicates X");
    ok(unique.abort(*ix));
    auto del=unique.begin();ok(unique.erase(*del,50,5));
    require(!unique.held_key(*del,50) && unique.held_key(*del,90)==LockMode::X,"unique Delete instant own key, commit successor");
    ok(unique.abort(*del));

    Index duplicates(16,false);duplicates.load({{10,1,1},{50,5,5},{50,6,6},{90,9,9}});
    auto many=duplicates.begin();ok(duplicates.erase(*many,50,5));
    require(duplicates.held_key(*many,50)==LockMode::X && !duplicates.held_key(*many,90),"multiple-instance Delete skips successor");
    ok(duplicates.abort(*many));
    auto one=duplicates.begin();ok(duplicates.erase(*one,10,1));
    require(duplicates.held_key(*one,10)==LockMode::X && duplicates.held_key(*one,50)==LockMode::X,"nonunique first value retains X for possible preceding duplicates");
    ok(duplicates.abort(*one));

    // Both unchanged-VN and changed-VN equality scan termination must avoid
    // locking EOF; another RID of the same value needs no extra S request.
    for(bool modify:{false,true}) {
        Index last(16,false);last.load({{50,1,1},{50,2,2}});
        auto tx=last.begin();Index::Cursor cursor;
        ok(last.open(*tx,50,Start::Equal,50,Stop::Equal,cursor,row));
        if(modify)ok(last.insert(*tx,{50,3,3}));
        const auto before=last.metrics().lock_requests;
        ok(last.next(*tx,cursor,row));require(row.rid==2,"next duplicate RID");
        if(modify){ok(last.next(*tx,cursor,row));require(row.rid==3,"own appended duplicate RID");}
        require(last.next(*tx,cursor,row)==Status::NotFound,"equality scan terminates at EOF");
        require(last.metrics().lock_requests==before && !last.held_key(*tx,0,true),"duplicate traversal and equality EOF need no extra key lock");
        ok(last.abort(*tx));
    }
    // A negative Fetch really does lock EOF, and inserts must trip on it.
    Index end(8,false,100ms);end.load({{10,1,1}});
    auto reader=end.begin();require(end.fetch(*reader,20,Start::Equal,row)==Status::NotFound,"negative EOF lookup");
    auto writer=end.begin();require(end.insert(*writer,{20,2,2})==Status::Timeout,"EOF S blocks insertion at end");
    ok(end.abort(*writer));ok(end.commit(*reader));
    auto after=end.begin();ok(end.insert(*after,{20,2,2}));ok(end.commit(*after));
}

void compatible_inserters() {
    Index index(8,false);index.load({{10,1,1},{90,9,9}});
    auto first=index.begin();auto second=index.begin();
    ok(index.insert(*first,{50,5,5}));ok(index.insert(*second,{50,6,6}));
    require(index.held_key(*first,50)==LockMode::IX && index.held_key(*second,50)==LockMode::IX,"IX/IX permits same-key different-RID insertion");
    require(!index.held_key(*first,90),"instant successor IX leaves no holding");
    ok(index.abort(*first));ok(index.commit(*second));
    require(index.inspect()==std::vector<Entry>({{10,1,1},{50,6,6},{90,9,9}}),"undo removes only own RID");
}

void replication_interleaving() {
    Index index(8,false,150ms);index.load({{10,1,1},{50,5,5}});
    auto t1=index.begin();Entry row;ok(index.fetch(*t1,20,Start::GreaterEqual,row));
    ok(index.insert(*t1,{30,3,3}));require(index.held_key(*t1,30)==LockMode::X,"replicate S successor to X inserted value");
    auto t2=index.begin();Signal waiting;
    index.set_observer([&](const Event& e){if(e.txn==t2->id() && e.phase==Phase::ReleaseForWait && e.key==30){require(e.page_latches==0,"wait retains no latch");waiting.set();}});
    auto action=std::async(std::launch::async,[&]{return index.insert(*t2,{25,2,2});});
    waiting.wait();require(action.wait_for(20ms)==std::future_status::timeout,"replicated lock blocks insertion behind own new boundary");
    ok(index.commit(*t1));ok(action.get());ok(index.commit(*t2));require(index.validate(),"replication schedule valid");
}

void figure4_reposition() {
    // order 4: leaf [10], leaf [30], leaf [50,70]. Input 20 visits [10]
    // first, then waits on 30 in the second leaf. While waiting, writer puts
    // 25 in the first leaf without changing the page that contained 30.
    Index index(4,false);index.load({{10,1,1},{30,3,3},{50,5,5},{70,7,7}});
    auto writer=index.begin();ok(index.insert(*writer,{30,30,30}));
    auto reader=index.begin();Signal waiting;std::atomic<unsigned> revalidated{0};
    index.set_observer([&](const Event& e){
        if(e.txn==reader->id() && e.phase==Phase::ReleaseForWait && e.key==30)waiting.set();
        if(e.txn==reader->id() && e.phase==Phase::Revalidate)++revalidated;
        if(e.phase==Phase::ReleaseForWait)require(e.page_latches==0,"Figure 4 latch release");
    });
    Entry result;auto future=std::async(std::launch::async,[&]{return index.fetch(*reader,20,Start::GreaterEqual,result);});
    waiting.wait();ok(index.insert(*writer,{25,25,25}));ok(index.commit(*writer));
    ok(future.get());require(result.key==25 && revalidated>0,"Figure 4 must rediscover key in predecessor page");
    require(!index.held_key(*reader,30) && index.held_key(*reader,25)==LockMode::S,"manual obsolete key lock released");
    ok(index.commit(*reader));
}

void delete_wait_and_abort() {
    Index index(4,true);index.load({{10,1,1},{30,3,3},{50,5,5},{70,7,7}});
    auto deleter=index.begin();ok(index.erase(*deleter,10,1));
    require(index.held_key(*deleter,30)==LockMode::X,"Delete X-locks next value until termination");
    auto reader=index.begin();Signal waiting;
    index.set_observer([&](const Event& e){if(e.txn==reader->id() && e.phase==Phase::ReleaseForWait && e.key==30)waiting.set();});
    Entry row;auto future=std::async(std::launch::async,[&]{return index.fetch(*reader,10,Start::GreaterEqual,row);});
    waiting.wait();ok(index.abort(*deleter));ok(future.get());require(row.key==10,"reader sees restored deletion after undo/reposition");
    ok(index.commit(*reader));require(index.validate(),"page deletion/undo tree valid");
    require(index.metrics().page_deletes>0,"exercise physical empty-leaf removal");
}

void cursor_after_own_change() {
    Index index(4,false);index.load({{10,1,1},{30,3,3},{50,5,5},{70,7,7}});
    auto tx=index.begin();Index::Cursor cursor;Entry row;
    ok(index.open(*tx,10,Start::GreaterEqual,80,Stop::Less,cursor,row));
    ok(index.erase(*tx,10,1));ok(index.insert(*tx,{20,2,2}));
    ok(index.next(*tx,cursor,row));require(row.key==20,"VN repositions deleted current cursor");
    ok(index.abort(*tx));require(index.validate(),"cursor change undo");
}

void deadlock_and_timeout() {
    Index index(8,false,100ms);index.load({{10,1,1},{30,3,3},{50,5,5}});
    auto t1=index.begin();auto t2=index.begin();Entry row;
    ok(index.fetch(*t1,10,Start::Equal,row));ok(index.fetch(*t2,30,Start::Equal,row));
    Signal wait;
    index.set_observer([&](const Event& e){if(e.txn==t1->id() && e.phase==Phase::ReleaseForWait && e.key==30)wait.set();});
    auto f=std::async(std::launch::async,[&]{return index.erase(*t1,30,3);});wait.wait();
    const auto cycle=index.erase(*t2,10,1);
    require(cycle==Status::Deadlock,"detect lock conversion cycle without page latch deadlock");
    ok(index.abort(*t2));ok(f.get());ok(index.abort(*t1));require(index.validate(),"deadlock rollback");
    auto holding=index.begin();ok(index.fetch(*holding,30,Start::Equal,row));
    auto blocked=index.begin();require(index.erase(*blocked,30,3)==Status::Timeout,"bounded lock wait");
    ok(index.abort(*blocked));ok(index.commit(*holding));
}

void concurrent_splits_and_undo() {
    Index index(8,false,1000ms);index.load({{0,1,1},{UINT64_MAX,2,2}});
    std::atomic<unsigned> bad{0};std::vector<std::thread> workers;
    for(unsigned w=0;w<8;++w)workers.emplace_back([&,w] {
        for(unsigned n=0;n<200;++n) {
            const auto key=100+((n*37+w*11)%400);
            auto tx=index.begin();auto status=index.insert(*tx,{key,1000+w*200+n,n});
            if(status!=Status::Ok){++bad;index.abort(*tx);continue;}
            if(n%4==0)index.abort(*tx);else index.commit(*tx);
        }
    });
    for(auto& thread:workers)thread.join();
    require(bad==0 && index.size()==1202,"concurrent inserted/aborted RID counts");
    require(index.validate() && index.metrics().live_lock_names==0,"concurrent SMO structure and lock cleanup");
    require(index.metrics().max_page_latches<=2,"at most two page latches");
}
void random_model_and_empty_tree() {
    for(bool unique:{false,true})for(unsigned seed=0;seed<8;++seed) {
        Index index(4,unique);std::map<std::pair<idx_key_t,rid_t>,Entry> model;
        std::mt19937_64 rng(seed);
        for(unsigned iteration=0;iteration<300;++iteration) {
            auto tx=index.begin();auto before=model;
            for(unsigned action=0;action<5;++action) {
                const auto key=rng()%24,rid=unique?key:rng()%80;
                const auto pair=std::make_pair(key,rid);
                if(rng()%2) {
                    const Entry entry{key,rid,rng()};const auto status=index.insert(*tx,entry);
                    if(model.count(pair))require(status==Status::Duplicate,"model duplicate");
                    else {ok(status);model[pair]=entry;}
                } else {
                    auto status=index.erase(*tx,key,rid);
                    if(model.erase(pair))ok(status);else require(status==Status::NotFound,"model missing delete");
                }
            }
            if(rng()%3==0){ok(index.abort(*tx));model=before;}else ok(index.commit(*tx));
            std::vector<Entry> expected;for(const auto& row:model)expected.push_back(row.second);
            require(index.validate() && index.inspect()==expected,"random transactional model including abort");
        }
        auto empty=index.begin();for(const auto& row:model)ok(index.erase(*empty,row.second.key,row.second.rid));
        ok(index.commit(*empty));require(index.size()==0 && index.height()==1 && index.validate(),"delete propagates to stable empty root");
        auto reload=index.begin();ok(index.insert(*reload,{999,999,999}));ok(index.commit(*reload));require(index.validate(),"reuse root after complete deletion");
    }
}
void concurrent_repeatable_scans() {
    Index index(8,false,500ms);index.load({{0,1,1},{1000,2,2}});
    std::atomic<unsigned> failures{0},committed_rows{0},read_commits{0};std::vector<std::thread> threads;
    for(unsigned worker=0;worker<4;++worker)threads.emplace_back([&,worker] {
        for(unsigned n=0;n<100;++n) {
            const Entry entry{1+(n*17+worker*7)%100,1000+worker*100+n,n};
            auto tx=index.begin();auto status=index.insert(*tx,entry);
            if(status==Status::Ok && n%5==0)status=index.erase(*tx,entry.key,entry.rid);
            if(status!=Status::Ok || n%4==0)index.abort(*tx);
            else {index.commit(*tx);if(n%5!=0)++committed_rows;}
        }
    });
    for(unsigned worker=0;worker<4;++worker)threads.emplace_back([&] {
        for(unsigned n=0;n<80;++n) {
            auto tx=index.begin();std::vector<Entry> first,second;
            auto status=index.scan(*tx,1,101,first);
            if(status==Status::Ok)status=index.scan(*tx,1,101,second);
            if(status!=Status::Ok)index.abort(*tx);
            else {if(first!=second)++failures;index.commit(*tx);++read_commits;}
        }
    });
    for(auto& thread:threads)thread.join();
    require(failures==0 && read_commits>0,"concurrent repeatable range scans with actual insert/delete/undo");
    require(index.size()==committed_rows+2 && index.validate(),"mixed reader/writer structure and count");
    require(index.metrics().live_lock_names==0 && index.metrics().max_page_latches<=2,"mixed cleanup and latch bound");
}
int main() {
    try {
        for(const auto& test:std::vector<std::pair<const char*,void(*)()>>{
            {"basic_and_undo",basic_and_undo},{"duplicate_cursor",duplicate_cursor},
            {"figure1_modes_eof",figure1_modes_and_eof},{"compatible_inserters",compatible_inserters},{"replication",replication_interleaving},
            {"figure4",figure4_reposition},{"delete_wait_abort",delete_wait_and_abort},
            {"cursor_own_change",cursor_after_own_change},{"deadlock",deadlock_and_timeout},
            {"concurrent_splits",concurrent_splits_and_undo},{"random_model",random_model_and_empty_tree},
            {"repeatable_scans",concurrent_repeatable_scans}}) {
            std::cout<<"RUN "<<test.first<<std::endl;test.second();std::cout<<"PASS "<<test.first<<std::endl;
        }
    } catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
