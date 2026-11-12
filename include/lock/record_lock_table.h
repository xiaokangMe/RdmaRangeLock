#pragma once

#include "lock/wait_policy.h"
#include "common/execution_context.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <vector>

namespace rrl {

// Local record-only authority. No ranges, shard gates, holder allocations or
// virtual lock interface. One context owns one bit and runs one attempt at once.
class RecordLockTable {
public:
    using Clock = std::chrono::steady_clock;
    using Word = std::atomic<std::uint64_t>;
    static constexpr std::uint64_t exclusive = 1ULL << 63;
    static constexpr std::size_t max_contexts = 64;
    static constexpr std::uint64_t max_token = (1ULL << 61) - 1;
    enum State : std::uint64_t { Idle, Running, Wounded, Committing };
    struct alignas(64) Slot {
        Word control{0}, age{0};
    };

    RecordLockTable(std::size_t dense_keys, std::vector<idx_key_t> sparse_keys,
                    std::size_t contexts)
        : dense_(dense_keys), contexts_(contexts), sparse_(std::move(sparse_keys)) {
        if (!contexts || contexts > max_contexts) throw std::invalid_argument("compact record table requires 1..64 contexts");
        std::sort(sparse_.begin(), sparse_.end());
        sparse_.erase(std::remove_if(sparse_.begin(), sparse_.end(),
            [&](idx_key_t key) { return key < dense_; }), sparse_.end());
        sparse_.erase(std::unique(sparse_.begin(), sparse_.end()), sparse_.end());
        if (dense_ > SIZE_MAX / sizeof(Word) || sparse_.size() > SIZE_MAX / sizeof(Word) - dense_)
            throw std::invalid_argument("record table capacity overflow");
        words_.reset(new Word[dense_ + sparse_.size()]{});
        slots_.reset(new Slot[contexts]);
    }
    RecordLockTable(const RecordLockTable&) = delete;
    RecordLockTable& operator=(const RecordLockTable&) = delete;

    Word* locate(idx_key_t key) const noexcept {
        if (key < dense_) return words_.get() + key;
        const auto it = std::lower_bound(sparse_.begin(), sparse_.end(), key);
        return it != sparse_.end() && *it == key ? words_.get() + dense_ + (it - sparse_.begin()) : nullptr;
    }
    std::size_t lock_bytes() const { return (dense_ + sparse_.size()) * sizeof(Word); }
    std::size_t directory_bytes() const { return sparse_.capacity() * sizeof(idx_key_t); }
    std::size_t state_bytes() const { return contexts_ * sizeof(Slot); }
    void poison() { poisoned_.store(true, std::memory_order_release); }
    bool healthy() const { return !poisoned_.load(std::memory_order_acquire); }
    bool empty() const {
        for (std::size_t n = 0; n < dense_ + sparse_.size(); ++n)
            if (words_[n].load(std::memory_order_relaxed)) return false;
        return true;
    }

    class Client {
    public:
        Client(RecordLockTable& table, std::size_t slot, std::size_t budget)
            : table_(table), slot_(slot), bit_(slot < 63 ? 1ULL << slot : 0), budget_(budget) {
            if (slot >= table.contexts_ || !budget) throw std::invalid_argument("invalid record context/budget");
            held_.reserve(budget); // Never allocate between granting and recording a lock.
        }
        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;
        ~Client() { if (id_ && table_.healthy()) abort(); }

        PolicyReply begin(std::uint64_t age, std::uint64_t budget_ns) {
            if (id_ || !held_.empty() || !table_.healthy() || !budget_ns || budget_ns > 600000000000ULL ||
                (age && age != age_)) return {};
            if (!age) age = table_.next_age_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (!age || generation_ >= (max_token - slot_) / table_.contexts_) return {};
            id_ = (++generation_) * table_.contexts_ + slot_;
            age_ = age;
            deadline_ = Clock::now() + std::chrono::nanoseconds(budget_ns);
            capacity_abort_ = false;
            table_.slots_[slot_].age.store(age_, std::memory_order_release);
            // All old reader bits have been removed before a new generation is published.
            table_.slots_[slot_].control.store(control(id_, Running), std::memory_order_release);
            return {TxnResult::Ok, id_, 0, age_};
        }
        txnid_t id() const { return id_; }
        Clock::time_point deadline() const { return deadline_; }
        std::size_t held_count() const { return held_.size(); }
        std::size_t held_bytes() const { return held_.capacity() * sizeof(Word*); }
        bool capacity_abort() const { return capacity_abort_; }
        TxnResult check() const {
            if (!id_ || !table_.healthy()) return TxnResult::Error;
            const auto word = table_.slots_[slot_].control.load(std::memory_order_acquire);
            if ((word >> 3) != id_) return TxnResult::Error;
            if ((word & 7) == Wounded) return TxnResult::Wounded;
            if ((word & 7) != Running && (word & 7) != Committing) return TxnResult::Error;
            return Clock::now() >= deadline_ ? TxnResult::Deadline : TxnResult::Ok;
        }

        template<WaitPolicy Policy>
        PolicyReply acquire(Word& record, LockMode mode) {
            if (!id_ || (mode != LockMode::S && mode != LockMode::X)) return {};
            // The 64th context has no free reader bit in the 64-bit state word.
            // Encode its shared lock as an exclusive owner to preserve safety.
            if (slot_ == 63 && mode == LockMode::S) mode = LockMode::X;
            auto word = record.load(std::memory_order_acquire);
            bool waited = false;
            Clock::time_point wait_start;
            unsigned collisions = 0;
            auto reply = [&](TxnResult result) {
                return PolicyReply{result, id_, waited ? static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - wait_start).count()) : 0};
            };
            for (;;) {
                if constexpr (Policy == WaitPolicy::WoundWait) {
                    if ((table_.slots_[slot_].control.load(std::memory_order_acquire) & 7) == Wounded)
                        return reply(TxnResult::Wounded);
                }
                if (word & exclusive) {
                    if ((word & ~exclusive) == id_) return reply(TxnResult::Ok);
                } else if (mode == LockMode::S && (word & bit_)) {
                    return reply(TxnResult::Ok);
                } else if ((mode == LockMode::S) || word == 0 || word == bit_) {
                    const bool already_held = (word & bit_) != 0;
                    if (!already_held && held_.size() == budget_) {
                        capacity_abort_ = true;
                        return reply(TxnResult::Conflict);
                    }
                    const auto desired = mode == LockMode::S ? word | bit_ : exclusive | id_;
                    if (record.compare_exchange_strong(word, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
                        if (!already_held) held_.push_back(&record);
                        if constexpr (Policy == WaitPolicy::WoundWait) {
                            if ((table_.slots_[slot_].control.load(std::memory_order_acquire) & 7) == Wounded)
                                return reply(TxnResult::Wounded);
                        }
                        return reply(TxnResult::Ok);
                    }
                    if constexpr (Policy != WaitPolicy::NoWait) {
                        if (++collisions == 32) {
                            const auto status = check();
                            if (status != TxnResult::Ok) return reply(status);
                            collisions = 0;
                        }
                    }
                    continue;
                }
                if constexpr (Policy == WaitPolicy::NoWait) {
                    return reply(TxnResult::Conflict);
                } else {
                    const auto status = check();
                    if (status != TxnResult::Ok) return reply(status);
                    if (!table_.healthy()) return reply(TxnResult::Error);
                    if (word & exclusive) {
                        if (!visit_holder<Policy>(word & ~exclusive)) return reply(TxnResult::Conflict);
                    } else {
                        auto readers = word & ~bit_;
                        while (readers) {
                            const auto other_slot = static_cast<unsigned>(__builtin_ctzll(readers));
                            const auto mask = 1ULL << other_slot;
                            readers &= readers - 1;
                            // Sandwich the CURRENT bit with generation reads. A bit
                            // from an old snapshot is not an identity for a reused slot.
                            const auto first = table_.slots_[other_slot].control.load(std::memory_order_acquire);
                            const auto current = record.load(std::memory_order_acquire);
                            const auto second = table_.slots_[other_slot].control.load(std::memory_order_acquire);
                            if (first == second && !(current & exclusive) && (current & mask) &&
                                !visit_holder<Policy>(first >> 3)) return reply(TxnResult::Conflict);
                        }
                    }
                    if (!waited) { waited = true; wait_start = Clock::now(); }
                    cooperative_yield();
                    word = record.load(std::memory_order_acquire);
                }
            }
        }

        template<WaitPolicy Policy>
        TxnResult prepare_commit() {
            const auto result = check();
            if (result != TxnResult::Ok) return result;
            if constexpr (Policy == WaitPolicy::WoundWait) {
                auto expected = control(id_, Running);
                if (!table_.slots_[slot_].control.compare_exchange_strong(expected,
                    control(id_, Committing), std::memory_order_acq_rel))
                    return expected == control(id_, Wounded) ? TxnResult::Wounded : TxnResult::Error;
            } else {
                table_.slots_[slot_].control.store(control(id_, Committing), std::memory_order_release);
            }
            return TxnResult::Ok;
        }
        // Caller completes data writeback before release. No search or allocation.
        void finish() noexcept {
            if (!id_) return;
            for (Word* record : held_) {
                const auto word = record->load(std::memory_order_relaxed);
                if (word & exclusive) record->store(0, std::memory_order_release);
                else record->fetch_and(~bit_, std::memory_order_release);
            }
            held_.clear();
            table_.slots_[slot_].control.store(control(id_, Idle), std::memory_order_release);
            id_ = 0;
        }
        void abort() noexcept { finish(); }

    private:
        template<WaitPolicy Policy>
        bool visit_holder(txnid_t holder) {
            if (!holder || holder == id_) return true;
            auto& slot = table_.slots_[holder % table_.contexts_];
            const auto first = slot.control.load(std::memory_order_acquire);
            const auto age = slot.age.load(std::memory_order_acquire);
            const auto second = slot.control.load(std::memory_order_acquire);
            if (first != second || (first >> 3) != holder || (first & 7) == Idle) return true;
            if constexpr (Policy == WaitPolicy::WaitDie) {
                return age_ <= age;
            } else {
                if (age_ < age && (first & 7) == Running) {
                    auto expected = first;
                    slot.control.compare_exchange_strong(expected, control(holder, Wounded), std::memory_order_acq_rel);
                }
                return true;
            }
        }
        static std::uint64_t control(txnid_t id, State state) { return (id << 3) | state; }
        RecordLockTable& table_;
        std::size_t slot_;
        const std::uint64_t bit_;
        std::size_t budget_;
        std::vector<Word*> held_;
        txnid_t id_{0};
        std::uint64_t generation_{0}, age_{0};
        Clock::time_point deadline_;
        bool capacity_abort_{false};
    };

private:
    const std::size_t dense_, contexts_;
    std::vector<idx_key_t> sparse_;
    std::unique_ptr<Word[]> words_;
    std::unique_ptr<Slot[]> slots_;
    alignas(64) Word next_age_{0};
    std::atomic<bool> poisoned_{false};
};
static_assert(sizeof(RecordLockTable::Word) == 8, "record state must be 8 bytes");
static_assert(RecordLockTable::Word::is_always_lock_free, "record CAS must be lock-free");

} // namespace rrl
