#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace resident {

// Single-producer single-consumer ring of trivially copyable items.
template <class T, size_t N> class SpscRing
{
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    bool push(const T& v)
    {
        const size_t w = w_.load(std::memory_order_relaxed);
        if (w - r_.load(std::memory_order_acquire) >= N)
            return false;
        buf_[w & (N - 1)] = v;
        w_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& v)
    {
        const size_t r = r_.load(std::memory_order_relaxed);
        if (r == w_.load(std::memory_order_acquire))
            return false;
        v = buf_[r & (N - 1)];
        r_.store(r + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<T, N> buf_{};
    std::atomic<size_t> w_{0}, r_{0};
};

} // namespace resident
