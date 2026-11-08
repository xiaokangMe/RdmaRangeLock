#include "lock/wait_policy.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <arpa/inet.h>
#include <endian.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rrl {
namespace {
enum Op : std::uint64_t { Begin=1,Acquire,Check,Prepare,End,Close };
constexpr std::uint64_t magic=0x52524c5741495434ULL;
using Message=std::array<std::uint64_t,16>;
bool transfer(int fd,void* data,std::size_t size,bool sending) {
    auto* p=static_cast<char*>(data);
    while(size) {
        const auto n=sending ? ::send(fd,p,size,MSG_NOSIGNAL) : ::recv(fd,p,size,0);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return false;
        p+=n;size-=static_cast<std::size_t>(n);
    }
    return true;
}
bool send_message(int fd,Message m) {
    for(auto& v:m)v=htobe64(v);
    return transfer(fd,m.data(),sizeof(m),true);
}
bool read_message(int fd,Message& m) {
    if(!transfer(fd,m.data(),sizeof(m),false))return false;
    for(auto& v:m)v=be64toh(v);
    return m[0]==magic;
}
void timeout(int fd,std::uint64_t ms) {
    timeval tv{static_cast<long>(ms/1000),static_cast<long>((ms%1000)*1000)};
    ::setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    ::setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
}
}
struct PolicyClient::Impl {
    TransactionCoordinator* local{nullptr};
    std::uint64_t session{0};
    int fd{-1};
    PolicyReply request(Message m) {
        auto fail=[&] {if(fd>=0){::shutdown(fd,SHUT_RDWR);::close(fd);fd=-1;}return PolicyReply{};};
        if(fd<0 || !send_message(fd,m))return fail();
        Message reply{};
        if(!read_message(fd,reply) || reply[1]>static_cast<std::uint64_t>(TxnResult::Error))return fail();
        return {static_cast<TxnResult>(reply[1]),reply[2],reply[3],reply[4]};
    }
};
PolicyClient::PolicyClient(TransactionCoordinator& local,std::uint64_t session):impl_(new Impl) {
    impl_->local=&local;impl_->session=session;
}
PolicyClient::PolicyClient(const std::string& host,std::uint16_t port,std::uint64_t timeout_ms):impl_(new Impl) {
    if(!timeout_ms || timeout_ms>605000)throw std::invalid_argument("invalid coordinator timeout");
    const int fd=::socket(AF_INET,SOCK_STREAM,0);
    if(fd<0)throw std::runtime_error("coordinator socket failed");
    timeout(fd,timeout_ms);
    int yes=1;::setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);
    if(::inet_pton(AF_INET,host.c_str(),&address.sin_addr)!=1 ||
       ::connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0) {
        ::close(fd);throw std::runtime_error("coordinator connect failed");
    }
    impl_->fd=fd;
}
PolicyClient::~PolicyClient() {
    if(impl_->local)impl_->local->disconnect(impl_->session);
    else if(impl_->fd>=0) {
        impl_->request(Message{magic,Close});
        ::close(impl_->fd);
    }
}
PolicyReply PolicyClient::begin(WaitPolicy policy,std::uint64_t age,std::uint64_t budget) {
    return impl_->local ? impl_->local->begin(impl_->session,policy,age,budget) :
        impl_->request(Message{magic,Begin,static_cast<std::uint64_t>(policy),age,budget});
}
PolicyReply PolicyClient::acquire(txnid_t id,LockMode mode,const KeyRangeEnt& r,LockKind kind) {
    if(impl_->local)return impl_->local->acquire(impl_->session,id,mode,r,kind);
    return impl_->request(Message{magic,Acquire,id,static_cast<std::uint64_t>(mode),static_cast<std::uint64_t>(kind),
        r.StartKey,r.EndKey,r.has_start,r.has_end,static_cast<std::uint64_t>(r.start_bound),static_cast<std::uint64_t>(r.end_bound)});
}
PolicyReply PolicyClient::check(txnid_t id) {
    return impl_->local ? impl_->local->check(impl_->session,id) : impl_->request(Message{magic,Check,id});
}
PolicyReply PolicyClient::prepare_commit(txnid_t id) {
    return impl_->local ? impl_->local->prepare_commit(impl_->session,id) : impl_->request(Message{magic,Prepare,id});
}
PolicyReply PolicyClient::end(txnid_t id) {
    return impl_->local ? impl_->local->end(impl_->session,id) : impl_->request(Message{magic,End,id});
}
int run_wait_coordinator(std::uint16_t port,std::size_t lock_budget,std::uint64_t idle_timeout_ms) {
    if(!idle_timeout_ms || idle_timeout_ms>605000)throw std::invalid_argument("idle timeout must be 1..600000 ms");
    const int listener=::socket(AF_INET,SOCK_STREAM,0);
    if(listener<0)return 2;
    int yes=1;::setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=INADDR_ANY;address.sin_port=htons(port);
    if(::bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || ::listen(listener,128)) {
        ::close(listener);return 2;
    }
    // Metadata coordinator only: client explicitly releases physical slots
    // before End. A disconnected active session poisons all future grants.
    auto engine=std::make_shared<TransactionCoordinator>(lock_budget);
    std::uint64_t session=0;
    std::cout<<"Wait coordinator listening on port "<<port<<std::endl;
    for(;;) {
        const int fd=::accept(listener,nullptr,nullptr);
        if(fd<0){if(errno==EINTR)continue;::close(listener);return 2;}
        timeout(fd,idle_timeout_ms);
        ::setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
        const auto sid=++session;
        std::thread([engine,fd,sid] {
            for(;;) {
                Message m{};
                if(!read_message(fd,m))break;
                PolicyReply reply;
                const auto op=m[1];
                if(op==Begin)reply=engine->begin(sid,static_cast<WaitPolicy>(m[2]),m[3],m[4]);
                else if(op==Acquire) {
                    KeyRangeEnt r;r.StartKey=m[5];r.EndKey=m[6];r.has_start=m[7]!=0;r.has_end=m[8]!=0;
                    r.start_bound=static_cast<Bound>(m[9]);r.end_bound=static_cast<Bound>(m[10]);
                    if(m[3]<=static_cast<std::uint64_t>(LockMode::UIX) && m[4]<=static_cast<std::uint64_t>(LockKind::GriffinUpdate) && m[7]<=1 && m[8]<=1 && m[9]<=1 && m[10]<=1)
                        reply=engine->acquire(sid,m[2],static_cast<LockMode>(m[3]),r,static_cast<LockKind>(m[4]));
                } else if(op==Check)reply=engine->check(sid,m[2]);
                else if(op==Prepare)reply=engine->prepare_commit(sid,m[2]);
                else if(op==End)reply=engine->end(sid,m[2]);
                else if(op==Close)reply={TxnResult::Ok,0,0};
                if(!send_message(fd,Message{magic,static_cast<std::uint64_t>(reply.result),reply.id,reply.wait_ns,reply.age}))break;
                if(op==Close)break;
            }
            engine->disconnect(sid);
            ::close(fd);
        }).detach();
    }
}
} // namespace rrl
