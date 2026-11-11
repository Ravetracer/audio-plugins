#pragma once

// A first-in, first-out queue with a fixed capacity and no allocation after
// construction, for queues the audio thread fills and drains (the MIDI
// cable into a cartridge or HerMIDI, the bytes a cartridge transmits).
// Single-threaded: it replaces std::deque, not a lock-free ring. A push into
// a full queue is refused and reported, never grown.

#include <array>
#include <cstddef>

namespace resident::emu {

template <class T, size_t N> class FixedQueue
{
    static_assert(N > 0 && (N & (N - 1)) == 0, "capacity must be a power of two");

public:
    static constexpr size_t capacity() { return N; }

    bool empty() const { return count_ == 0; }
    bool full() const { return count_ == N; }
    size_t size() const { return count_; }

    // False, and nothing stored, when the queue is full.
    bool push_back(const T& v)
    {
        if (count_ == N)
            return false;
        items_[(head_ + count_) & (N - 1)] = v;
        ++count_;
        return true;
    }
    void pop_front()
    {
        if (count_ == 0)
            return;
        head_ = (head_ + 1) & (N - 1);
        --count_;
    }
    void clear() { head_ = count_ = 0; }

    T& front() { return items_[head_]; }
    const T& front() const { return items_[head_]; }
    T& back() { return items_[(head_ + count_ - 1) & (N - 1)]; }
    const T& back() const { return items_[(head_ + count_ - 1) & (N - 1)]; }
    // The i-th element from the front.
    const T& operator[](size_t i) const { return items_[(head_ + i) & (N - 1)]; }

private:
    std::array<T, N> items_{};
    size_t head_ = 0, count_ = 0;
};

} // namespace resident::emu
