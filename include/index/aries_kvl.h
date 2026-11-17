#pragma once

#include "common/types.h"
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace rrl::aries {

// C. Mohan, ARIES/KVL, VLDB 1990, Figures 5-10 and sections 5.1-5.4.
// A separate in-memory implementation, not an adapter over ClassicLockTable.
enum class Duration { Instant, Manual, Commit };
enum class Status { Ok, NotFound, Duplicate, Deadlock, Timeout, Invalid };
enum class Start { Equal, GreaterEqual, Greater };
enum class Stop { Less, LessEqual, Equal };
enum class Phase {
    SearchLeaf, ConditionalLock, ReleaseForWait, WaitGranted, Revalidate,
    Inserted, Deleted, Replicated, SplitStarted, SplitPublished, PageDeleted,
    CursorFast, CursorReposition, UndoInsert, UndoDelete
};
struct Entry {
    idx_key_t key{0};
    rid_t rid{0};
    std::uint64_t value{0};
    bool operator==(const Entry& other) const {
        return key==other.key && rid==other.rid && value==other.value;
    }
};
struct Event {
    Phase phase;
    txnid_t txn;
    idx_key_t key;
    bool eof;
    std::uint64_t page, version;
    unsigned page_latches;
    LockMode mode;
    Duration duration;
};
struct Metrics {
    std::uint64_t lock_requests{0}, instant_requests{0}, lock_waits{0};
    std::uint64_t revalidations{0}, replications{0}, splits{0}, page_deletes{0};
    std::uint64_t physical_inserts{0}, physical_deletes{0}, undo_actions{0};
    std::uint64_t cursor_fast{0}, cursor_repositions{0};
    std::size_t live_lock_names{0}, peak_lock_names{0}, max_page_latches{0};
};

class Index {
    struct Impl;
public:
    class Transaction {
        friend class Index;
        friend struct Impl;
        struct State;
        std::unique_ptr<State> state_;
        explicit Transaction(std::unique_ptr<State>);
    public:
        ~Transaction();
        Transaction(const Transaction&)=delete;
        Transaction& operator=(const Transaction&)=delete;
        txnid_t id() const;
        bool active() const;
    };
    struct Cursor {
        txnid_t txn{0};
        Entry current;
        std::uint64_t page{0}, version{0};
        std::size_t slot{0};
        idx_key_t stop_key{0};
        Stop stop{Stop::Less};
        bool positioned{false}, done{false};
    };

    explicit Index(std::size_t order=128,bool unique=false,
                   std::chrono::milliseconds lock_timeout=std::chrono::seconds(5));
    ~Index();
    Index(const Index&)=delete;
    Index& operator=(const Index&)=delete;
    std::unique_ptr<Transaction> begin();
    Status commit(Transaction&);
    Status abort(Transaction&);
    Status insert(Transaction&,Entry);
    Status erase(Transaction&,idx_key_t,rid_t);
    Status fetch(Transaction&,idx_key_t,Start,Entry&);
    Status open(Transaction&,idx_key_t,Start,idx_key_t,Stop,Cursor&,Entry&);
    Status next(Transaction&,Cursor&,Entry&);
    Status scan(Transaction&,idx_key_t,idx_key_t,std::vector<Entry>&);
    // These quiescent inspection/initial-load methods are not transactional reads.
    void load(const std::vector<Entry>&);
    std::vector<Entry> inspect() const;
    bool validate() const;
    std::size_t size() const;
    std::size_t height() const;
    Metrics metrics() const;
    std::optional<LockMode> held_key(const Transaction&,idx_key_t,bool eof=false) const;
    void set_observer(std::function<void(const Event&)>);
private:
    std::unique_ptr<Impl> impl_;
};
} // namespace rrl::aries
