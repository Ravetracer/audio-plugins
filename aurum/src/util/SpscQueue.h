#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace aurum {

// Bounded lock-free single-producer/single-consumer queue.
template <typename T> class SpscQueue
{
public:
    explicit SpscQueue(size_t capacity) : buffer_(capacity + 1) {}

    bool push(const T& v)
    {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t next = (w + 1) % buffer_.size();
        if (next == read_.load(std::memory_order_acquire))
            return false;
        buffer_[w] = v;
        write_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out)
    {
        const size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire))
            return false;
        out = buffer_[r];
        read_.store((r + 1) % buffer_.size(), std::memory_order_release);
        return true;
    }

    bool empty() const { return read_.load(std::memory_order_acquire) == write_.load(std::memory_order_acquire); }

private:
    std::vector<T> buffer_;
    std::atomic<size_t> read_{0};
    std::atomic<size_t> write_{0};
};

} // namespace aurum
