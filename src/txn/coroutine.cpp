// Scheduling reference: Deneva system/worker_thread.cpp create_routines /
// master_routine; CQ yield sites follow system/txn.cpp cas_remote_content.
// System ucontext replaces Boost coroutine without an added dependency.
#include "txn/coroutine.h"
#include "common/execution_context.h"
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

namespace rrl {
namespace {
std::atomic<std::uint64_t> next_id{1};
struct Context {
    std::uint64_t id{next_id.fetch_add(1)};
    const void* index_owner{nullptr};
    void* index_session{nullptr};
};
struct Routine {
    ucontext_t context{};
    void* mapping{MAP_FAILED};
    std::size_t bytes{0};
    Context local;
    bool done{false};
    ~Routine() { if(mapping!=MAP_FAILED)munmap(mapping,bytes); }
};
struct Engine {
    ucontext_t master{};
    std::vector<std::unique_ptr<Routine>> routines;
    std::function<void(std::size_t)> body;
    std::exception_ptr error;
    std::size_t current{0};
    std::uint64_t switches{0};
};
thread_local Engine* running=nullptr;
thread_local Context outside;
Context& context() noexcept {return running ? running->routines[running->current]->local : outside;}
// Keep returns_twice bookkeeping out of the stack-allocation loop. The
// captured instruction/stack pointers are subsequently replaced by makecontext.
__attribute__((noinline)) void initialize_context(ucontext_t& context) {
    if(getcontext(&context))throw std::runtime_error("getcontext failed");
}
void enter_routine() {
    auto& e=*running;
    auto& r=*e.routines[e.current];
    try {e.body(e.current);} catch(...) {if(!e.error)e.error=std::current_exception();}
    r.done=true;
    // Returning transfers to uc_link, with every C++ frame already unwound.
}
}
struct CoroutineScheduler::Impl {Engine engine;};
CoroutineScheduler::CoroutineScheduler(std::size_t count,std::size_t stack_bytes):impl_(new Impl) {
    if(!count || count>4096 || stack_bytes<65536 || stack_bytes>16777216)
        throw std::invalid_argument("coroutine count/stack bounds");
    const auto page=static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    stack_bytes=(stack_bytes+page-1)/page*page;
    for(std::size_t i=0;i<count;++i) {
        auto r=std::make_unique<Routine>();
        r->bytes=stack_bytes+2*page;
        r->mapping=mmap(nullptr,r->bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(r->mapping==MAP_FAILED)throw std::runtime_error("coroutine stack mmap failed");
        if(mprotect(static_cast<char*>(r->mapping)+page,stack_bytes,PROT_READ|PROT_WRITE))
            throw std::runtime_error("coroutine stack protection failed");
        initialize_context(r->context);
        r->context.uc_stack.ss_sp=static_cast<char*>(r->mapping)+page;
        r->context.uc_stack.ss_size=stack_bytes;
        r->context.uc_link=&impl_->engine.master;
        makecontext(&r->context,enter_routine,0);
        impl_->engine.routines.push_back(std::move(r));
    }
}
CoroutineScheduler::~CoroutineScheduler()=default;
void CoroutineScheduler::run(const std::function<void(std::size_t)>& f) {
    auto& e=impl_->engine;
    if(running || e.body)throw std::logic_error("nested or repeated coroutine scheduler");
    e.body=f;running=&e;
    bool unfinished=true;
    while(unfinished) {
        unfinished=false;
        for(e.current=0;e.current<e.routines.size();++e.current) {
            auto& r=*e.routines[e.current];
            if(r.done)continue;
            unfinished=true;++e.switches;
            if(swapcontext(&e.master,&r.context))std::terminate();
        }
    }
    running=nullptr;
    if(e.error)std::rethrow_exception(e.error);
}
std::uint64_t CoroutineScheduler::switches() const {return impl_->engine.switches;}
bool coroutine_active() noexcept {return running!=nullptr;}
void cooperative_yield() {
    if(!running){std::this_thread::yield();return;}
    auto& e=*running;
    if(swapcontext(&e.routines[e.current]->context,&e.master))std::terminate();
}
std::uint64_t execution_context_id() noexcept {return context().id;}
const void*& execution_index_owner() noexcept {return context().index_owner;}
void*& execution_index_session() noexcept {return context().index_session;}
} // namespace rrl
