#pragma once
#include "../src/txn/dag_hierarchy.h"
#include "lock/object_lock_manager.h"
namespace dag_contract {
using namespace rrl;
inline void require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
template<class F> inline void rejected(F&& f){bool caught=false;try{f();}catch(const std::invalid_argument&){caught=true;}require(caught,"invalid DAG rejected");}
inline void topology() {
    using detail::LockDag;
    rejected([]{LockDag x({{0,{}},{1,{2}},{2,{1}}});});
    rejected([]{LockDag x({{0,{}},{1,{9}}});});
    rejected([]{LockDag x({{0,{}},{1,{0,0}}});});
    rejected([]{LockDag x({{0,{}},{1,{}}});});
    rejected([]{LockDag x({{0,{}},{1,{0},LockKind::Node,0}});});
    rejected([]{LockDag x({{0,{},LockKind::Record,10}});});
    rejected([]{detail::dag_bucket_id(UINT64_MAX,1,2);});
    rejected([]{detail::check_dag_geometry(16,2,2);});
}
inline void check(ObjectLockManager& locks) {
    constexpr txnid_t reader=65001,writer=65002;
    auto clear=[&]{require(locks.unlock_all(reader)==RCOK && locks.unlock_all(writer)==RCOK,"DAG cleanup");};
    auto acquire=[&](txnid_t id,const auto& plan) {
        for(const auto& r:plan){const auto rc=locks.lock(id,r.mode,r.range,r.kind);if(rc!=RCOK)return rc;}return RCOK;
    };
    detail::LockDag graph({{0,{}},{1,{0}},{2,{0}},{3,{1}},{4,{2}},
                          {100,{3,4},LockKind::Record,10},{101,{3,4},LockKind::Record,11}});
    rejected([&]{graph.read_path({0,3,100});});rejected([&]{graph.write_record(3);});
    const auto all=graph.write_record(100),scan=graph.read_path({0,2,4});
    require(all.size()==6 && all.front().mode==LockMode::IX && all.back().kind==LockKind::Record && all.back().range.StartKey==10,"all ancestry and canonical RID");
    require(acquire(reader,scan)==RCOK && acquire(writer,all)==AbortConflict,"honor other index parent");clear();
    require(acquire(writer,all)==RCOK && acquire(reader,scan)==AbortConflict,"other index reverse conflict");clear();
    require(acquire(reader,graph.read_path({0,1,3,100}))==RCOK && acquire(writer,graph.write_record(101))==RCOK,"different canonical record progresses");clear();
    require(acquire(reader,graph.read_path({0,2,4,100}))==RCOK && acquire(writer,graph.write_record(100))==AbortConflict,"same canonical record across paths");clear();
    TransactionTrace range{1,{{OperationKind::RangeRead,12,14,0}}},point{2,{{OperationKind::Insert,13,0,1}}};
    require(acquire(reader,detail::dag_plan(range,16,2,1))==RCOK && acquire(writer,detail::dag_plan(point,16,2,0))==AbortConflict,"generated DAG phantom protection");clear();
    TransactionTrace mixed{1,{{OperationKind::RangeRead,12,14,0},{OperationKind::Update,13,0,1}}};
    const auto m=detail::dag_plan(mixed,16,2,1);
    const auto cover=std::find_if(m.begin(),m.end(),[](const auto& q){return q.kind==LockKind::Node && q.range.StartKey==detail::dag_bucket_id(0,1,2);});
    require(cover!=m.end() && cover->mode==LockMode::SIX && m.back().kind==LockKind::Record && m.back().mode==LockMode::X,"DAG mixed SIX and canonical X");
    require(locks.get_lock_count()==0,"DAG no leaks");
}
}
