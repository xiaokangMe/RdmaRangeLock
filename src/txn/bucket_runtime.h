#pragma once
#include "hierarchical_plan.h"
namespace rrl::detail {
// Gray-style continuous logical protection. Historical predicates/records
// never disappear during a representation conversion. No structural latch.
class RuntimeBuckets {
    struct Bucket {KeyRangeEnt range;LockMode mode{LockMode::IS};bool writes{false};};
    struct History {HierarchyRequest request;bool held{false};};
    std::vector<Bucket> buckets_;
    std::vector<History> history_;
    std::size_t operations_{0};
    static bool same(const KeyRangeEnt& a,const KeyRangeEnt& b) {
        return a.StartKey==b.StartKey && a.EndKey==b.EndKey && a.has_start==b.has_start &&
            a.has_end==b.has_end && a.start_bound==b.start_bound && a.end_bound==b.end_bound;
    }
    template<class V,class T> static void push(V& out,T value) {
        if(out.size()==out.capacity())throw std::runtime_error("bucket conversion workspace exhausted");
        out.push_back(value);
    }
    History& remember(const HierarchyRequest& request) {
        auto it=std::find_if(history_.begin(),history_.end(),[&](const auto& h){return h.request.kind==request.kind && same(h.request.range,request.range);});
        if(it!=history_.end()){it->request.mode=lock_mode_join(it->request.mode,request.mode);return *it;}
        push(history_,History{request,false});return history_.back();
    }
public:
    void reserve(std::size_t buckets,std::size_t history){buckets_.reserve(buckets);history_.reserve(history);}
    void reset(){buckets_.clear();history_.clear();operations_=0;}
    template<class Acquire>
    bool acquire_operation(const Operation& op,const std::vector<HierarchyRequest>& plan,Acquire&& acquire) {
        for(const auto& request:plan) {
            if(request.phase==0){if(!acquire(request.mode,request.range,request.kind))return false;continue;}
            if(request.phase==1) {
                auto b=std::find_if(buckets_.begin(),buckets_.end(),[&](const auto& x){return same(x.range,request.range);});
                if(!acquire(request.mode,request.range,request.kind))return false;
                if(b==buckets_.end()){push(buckets_,Bucket{request.range,request.mode,request.mode==LockMode::IX || request.mode==LockMode::SIX});}
                else {b->mode=lock_mode_join(b->mode,request.mode);b->writes|=request.mode==LockMode::IX || request.mode==LockMode::SIX;}
            }else {
                auto& h=remember(request);
                const auto b=std::find_if(buckets_.begin(),buckets_.end(),[&](const auto& x){return x.range.contains(request.range.StartKey);});
                const bool covered=b!=buckets_.end() && (b->mode==LockMode::X ||
                    (request.mode==LockMode::S && (b->mode==LockMode::S || b->mode==LockMode::SIX)));
                if(!covered){if(!acquire(h.request.mode,h.request.range,h.request.kind))return false;h.held=true;}
            }
        }
        if(op.kind==OperationKind::RangeRead && op.key<op.end_key)
            remember({LockMode::S,KeyRangeEnt::half_open(op.key,op.end_key),LockKind::HierarchyPredicate,2});
        return true;
    }
    template<class Acquire,class Convert,class Release>
    bool finish_operation(const PolicyRunConfig& cfg,PolicyStats& stats,Acquire&& acquire,Convert&& convert,Release&& release) {
        ++operations_;
        const bool fine=cfg.hierarchy_deescalate_after && operations_>=cfg.hierarchy_deescalate_after;
        if(fine) {
            // Restore every historical predicate, including empty scan
            // results, before weakening even one parent cover.
            for(auto& h:history_) {
                if(!acquire(h.request.mode,h.request.range,h.request.kind))return false;
                h.held=true;
            }
        }
        for(auto& b:buckets_) {
            if(fine) {
                if(lock_mode_is_intent(b.mode))continue;
                const auto target=b.writes?LockMode::IX:LockMode::IS;
                ++stats.deescalation_attempts;
                const auto rc=convert(target,b.range);
                if(rc==ERROR){++stats.deescalation_failures;return false;}
                if(rc!=RCOK){++stats.deescalation_failures;continue;}
                b.mode=target;++stats.deescalations;
            }else if(cfg.hierarchy_escalation_threshold) {
                const auto target=b.writes?LockMode::X:LockMode::S;
                if(b.mode==target || b.mode==LockMode::X)continue;
                const auto count=std::count_if(history_.begin(),history_.end(),[&](const auto& h){
                    return h.request.kind==LockKind::Record && b.range.contains(h.request.range.StartKey);
                });
                if(static_cast<std::size_t>(count)<cfg.hierarchy_escalation_threshold)continue;
                ++stats.escalation_attempts;
                const auto rc=convert(target,b.range);
                if(rc==ERROR){++stats.escalation_failures;return false;}
                if(rc!=RCOK){++stats.escalation_failures;continue;}
                b.mode=target;++stats.escalations;
                for(auto& h:history_)if(h.held && h.request.kind==LockKind::Record && b.range.contains(h.request.range.StartKey)) {
                    if(!release(h.request))return false;
                    h.held=false;
                }
            }
        }
        return true;
    }
};
}
