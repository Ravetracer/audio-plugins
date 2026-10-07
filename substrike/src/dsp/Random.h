#pragma once

#include <cstdint>

namespace substrike::dsp {

// xorshift64*: small, fast, and identical on every platform, so a seeded
// render is reproducible bit for bit.
class Rng
{
public:
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) { seed_(seed); }

    void seed_(uint64_t seed)
    {
        // splitmix64 spreads a small seed over all bits; zero is not allowed.
        uint64_t z = seed + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        state_ = (z ^ (z >> 31)) | 1ull;
    }

    uint64_t next()
    {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1Dull;
    }

    // [0, 1)
    double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    // [-1, 1)
    double bipolar() { return 2.0 * unit() - 1.0; }

private:
    uint64_t state_ = 1;
};

} // namespace substrike::dsp
