#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace aurum::dsp {

// Circular delay buffer with power-of-two length.
class DelayLine
{
public:
    void allocate(int maxDelaySamples)
    {
        int size = 1;
        while (size < maxDelaySamples + 8)
            size <<= 1;
        buffer_.assign(static_cast<size_t>(size), 0.0f);
        mask_ = size - 1;
        write_ = 0;
    }

    void clear() { std::fill(buffer_.begin(), buffer_.end(), 0.0f); }

    int capacity() const { return mask_ - 4; }

    void push(float x)
    {
        buffer_[static_cast<size_t>(write_)] = x;
        write_ = (write_ + 1) & mask_;
    }

    // Delay of 1 returns the most recently pushed sample.
    float readInt(int delay) const { return buffer_[static_cast<size_t>((write_ - delay) & mask_)]; }

    float readLinear(float delay) const
    {
        const int i = static_cast<int>(delay);
        const float f = delay - static_cast<float>(i);
        const float a = readInt(i);
        const float b = readInt(i + 1);
        return a + (b - a) * f;
    }

    // 4-point, 3rd-order Hermite interpolation (used outside feedback loops).
    float readHermite(float delay) const
    {
        const int i = static_cast<int>(delay);
        const float f = delay - static_cast<float>(i);
        const float xm1 = readInt(i - 1);
        const float x0 = readInt(i);
        const float x1 = readInt(i + 1);
        const float x2 = readInt(i + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }

    // Read pointer for the newest sample (index of last written).
    float* data() { return buffer_.data(); }
    int mask() const { return mask_; }
    int writePos() const { return write_; }

private:
    std::vector<float> buffer_{0.0f};
    int mask_ = 0;
    int write_ = 0;
};

// First-order allpass fractional delay reader. Magnitude response is flat,
// so it adds no loss inside feedback loops (Dattorro, "Effect Design" part 2).
struct AllpassInterpolator
{
    float yPrev = 0.0f;

    void reset() { yPrev = 0.0f; }

    float read(const DelayLine& line, float delay)
    {
        // Keep fractional part in [0.5, 1.5) for a well-behaved coefficient.
        float base = std::floor(delay - 0.5f);
        float d = delay - base;
        const int m = static_cast<int>(base);
        const float eta = (1.0f - d) / (1.0f + d);
        const float x0 = line.readInt(m);
        const float x1 = line.readInt(m + 1);
        const float y = eta * x0 + x1 - eta * yPrev;
        yPrev = y;
        return y;
    }
};

// Schroeder allpass section with an internal delay line.
class AllpassDiffuser
{
public:
    void allocate(int maxDelay) { line_.allocate(maxDelay); }
    void clear() { line_.clear(); }

    // Integer delay version.
    float process(float x, int delay, float g)
    {
        const float d = line_.readInt(delay);
        const float v = x + g * d;
        line_.push(v);
        return d - g * v;
    }

    // Fractional (linear interpolated) delay, for gliding/modulated diffusers.
    float processFrac(float x, float delay, float g)
    {
        const float d = line_.readLinear(delay);
        const float v = x + g * d;
        line_.push(v);
        return d - g * v;
    }

    // Normalised (orthogonal) junction: same allpass transfer function, but the
    // scattering is energy preserving, so modulating the delay cannot pump
    // energy into a feedback loop. Hermite read (|H| <= 1, i.e. passive).
    // Requires delay >= 2.
    float processNormalised(float x, float delay, float g)
    {
        const float c = std::sqrt(1.0f - g * g);
        const float d = line_.readHermite(delay);
        line_.push(c * x + g * d);
        return c * d - g * x;
    }

    const DelayLine& line() const { return line_; }

private:
    DelayLine line_;
};

} // namespace aurum::dsp
