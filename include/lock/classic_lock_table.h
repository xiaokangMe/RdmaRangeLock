#pragma once
#include "lock/record_lock_table.h"
#include "txn/workload.h"

namespace rrl {
// Stable ordered coordinate directory: points and the open gaps between them.
// Register possible keys, NOT their presence/absence or transaction schedule.
// Future inserts split neither a lock name nor an already-held interval.
class ClassicLockTable {
public:
    using Client=RecordLockTable::Client;
    ClassicLockTable(std::vector<idx_key_t> keys,std::size_t contexts,PolicyProtocol protocol)
        : keys_(normalize(std::move(keys))), protocol_(protocol),
          authority_(resource_count(keys_.size(),protocol),{},contexts) {
        if(protocol!=PolicyProtocol::NextKey && protocol!=PolicyProtocol::PriorKey && protocol!=PolicyProtocol::Gap)
            throw std::invalid_argument("invalid classic lock protocol");
        while(dense_<keys_.size() && keys_[dense_]==dense_)++dense_;
        // Dense coordinates are implicit. Retain only the sparse suffix,
        // including releasing the temporary trace-registration capacity.
        std::vector<idx_key_t>(keys_.begin()+dense_,keys_.end()).swap(keys_);
    }
    RecordLockTable& authority(){return authority_;}
    std::size_t directory_bytes()const{return keys_.capacity()*sizeof(idx_key_t);}
    std::size_t key_count()const{return dense_+keys_.size();}

    template<WaitPolicy Policy>
    PolicyReply acquire(Client& client,LockMode mode,const KeyRangeEnt& range,LockKind kind) {
        PolicyReply reply{TxnResult::Ok,client.id(),0};
        if(!range.valid() || (mode!=LockMode::S && mode!=LockMode::X))return {};
        auto take=[&](std::size_t resource) {
            const auto r=client.acquire<Policy>(*authority_.locate(resource),mode);
            reply.result=r.result;reply.wait_ns+=r.wait_ns;
            return r.result==TxnResult::Ok;
        };
        if(protocol_==PolicyProtocol::Gap) {
            // Record and insert-intent conflict on actual key ownership.
            // Gap guards only conflict with insert-intent, never with records.
            auto first=lower(range.StartKey);
            if(kind==LockKind::Record || kind==LockKind::InsertIntent) {
                if(!range.has_start || !range.has_end || range.StartKey!=range.EndKey ||
                   range.start_bound!=Bound::Closed || range.end_bound!=Bound::Closed ||
                   (kind==LockKind::InsertIntent && mode!=LockMode::X))return {};
                if(!registered(range.StartKey,first))return {};
                const auto rank=first;
                if(kind==LockKind::InsertIntent && !take(key_count()+rank))return reply;
                take(rank);return reply;
            }
            if(kind!=LockKind::Gap || mode!=LockMode::S)return {};
            first=range.has_start ? (range.start_bound==Bound::Closed ?
                lower(range.StartKey):upper(range.StartKey)) : 0;
            const auto last=range.has_end ? (range.end_bound==Bound::Closed ?
                upper(range.EndKey):lower(range.EndKey)) : key_count();
            for(auto rank=first;rank!=last;++rank)if(!take(key_count()+rank))break;
            return reply;
        }
        if(kind!=(protocol_==PolicyProtocol::NextKey?LockKind::NextKey:LockKind::PriorKey))return {};
        const auto lo=range.has_start ? endpoint(range.StartKey,range.start_bound,true):0;
        const auto hi=range.has_end ? endpoint(range.EndKey,range.end_bound,false):2*key_count();
        for(auto resource=lo;resource<=hi;++resource)if(!take(resource))break;
        return reply;
    }
private:
    std::size_t lower(idx_key_t key)const {
        return key<dense_ ? static_cast<std::size_t>(key) : dense_+
            static_cast<std::size_t>(std::lower_bound(keys_.begin(),keys_.end(),key)-keys_.begin());
    }
    std::size_t upper(idx_key_t key)const {
        return key<dense_ ? static_cast<std::size_t>(key)+1 : dense_+
            static_cast<std::size_t>(std::upper_bound(keys_.begin(),keys_.end(),key)-keys_.begin());
    }
    bool registered(idx_key_t key,std::size_t rank)const {
        return key<dense_ || (rank<key_count() && keys_[rank-dense_]==key);
    }
    static std::vector<idx_key_t> normalize(std::vector<idx_key_t> keys) {
        std::sort(keys.begin(),keys.end());keys.erase(std::unique(keys.begin(),keys.end()),keys.end());return keys;
    }
    static std::size_t resource_count(std::size_t keys,PolicyProtocol protocol) {
        if(keys>(SIZE_MAX-1)/2)throw std::invalid_argument("classic lock directory overflow");
        return protocol==PolicyProtocol::Gap ? std::max<std::size_t>(2*keys,1):2*keys+1;
    }
    std::size_t endpoint(idx_key_t key,Bound bound,bool start)const {
        const auto rank=lower(key);
        if(!registered(key,rank))throw std::invalid_argument("unregistered classic endpoint");
        const auto point=2*rank+1;
        return bound==Bound::Closed ? point : start ? point+1 : point-1;
    }
    std::vector<idx_key_t> keys_;
    std::size_t dense_{0};
    PolicyProtocol protocol_;
    RecordLockTable authority_;
};
} // namespace rrl
