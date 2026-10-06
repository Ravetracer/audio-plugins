#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace aurum::dsp {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr float kPiF = 3.14159265358979323846f;

inline float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }
inline double dbToGain(double db) { return std::pow(10.0, db * 0.05); }
inline float gainToDb(float g) { return 20.0f * std::log10(std::max(g, 1e-30f)); }

template <typename T> inline T clamp(T v, T lo, T hi) { return std::min(std::max(v, lo), hi); }

template <typename T> inline T lerp(T a, T b, T t) { return a + (b - a) * t; }

// Shelf Q as shown to the user vs. the analog prototype Q: a shelf with user
// Q q behaves like the prototype with q * sqrt(2) (matches the curve shape
// of common shelving EQs; Q 0.707 gives a smooth shelf without overshoot).
constexpr double kShelfQScale = 1.41421356237;

// Geometric interpolation between two positive values.
inline double gerp(double a, double b, double t) { return a * std::pow(b / a, t); }

// Equal-power crossfade gains for t in [0,1].
inline void equalPower(float t, float& a, float& b)
{
    a = std::cos(t * 0.5f * kPiF);
    b = std::sin(t * 0.5f * kPiF);
}

// One-pole smoothing coefficient for a time constant in seconds.
inline float onePoleCoeff(double seconds, double sampleRate)
{
    if (seconds <= 0.0)
        return 0.0f;
    return static_cast<float>(std::exp(-1.0 / (seconds * sampleRate)));
}

// Small deterministic PRNG (xorshift32), used for reproducible patterns.
struct Rng
{
    uint32_t state;
    Rng() : state(0x9e3779b9u) {}
    explicit Rng(uint32_t seed) : state(seed ? seed : 0x9e3779b9u) {}
    uint32_t next()
    {
        uint32_t x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }
    // Uniform in [0,1).
    float uniform() { return (next() >> 8) * (1.0f / 16777216.0f); }
    // Uniform in [-1,1).
    float bipolar() { return uniform() * 2.0f - 1.0f; }
};

// Linear ramp smoother for per-sample gains.
struct LinearSmoother
{
    float current = 0.0f;
    float target = 0.0f;
    float step = 0.0f;
    int remaining = 0;

    void reset(float v)
    {
        current = target = v;
        step = 0.0f;
        remaining = 0;
    }
    void setTarget(float v, int rampSamples)
    {
        if (v == target && remaining == 0)
            return;
        target = v;
        if (rampSamples <= 0)
        {
            current = v;
            remaining = 0;
            step = 0.0f;
            return;
        }
        remaining = rampSamples;
        step = (target - current) / static_cast<float>(rampSamples);
    }
    float next()
    {
        if (remaining > 0)
        {
            current += step;
            if (--remaining == 0)
                current = target;
        }
        return current;
    }
    bool isRamping() const { return remaining > 0; }
};

// One-pole exponential smoother, advanced in blocks.
struct OnePoleSmoother
{
    double value = 0.0;
    double coeff = 0.0;
    void setTime(double seconds, double rate) { coeff = seconds > 0 ? std::exp(-1.0 / (seconds * rate)) : 0.0; }
    void reset(double v) { value = v; }
    double process(double target)
    {
        value = target + (value - target) * coeff;
        return value;
    }
};

// First-order antiderivative anti-aliased tanh saturation.
struct AdaaTanh
{
    float x1 = 0.0f;
    float f1 = 0.0f; // antiderivative at x1: log(cosh(x1))

    static float logCosh(float x)
    {
        const float ax = std::fabs(x);
        // log(cosh(x)) = |x| + log1p(exp(-2|x|)) - log(2)
        return ax + std::log1p(std::exp(-2.0f * ax)) - 0.69314718f;
    }
    void reset()
    {
        x1 = 0.0f;
        f1 = 0.0f;
    }
    float process(float x)
    {
        const float fx = logCosh(x);
        const float dx = x - x1;
        float y;
        if (std::fabs(dx) < 1e-4f)
            y = std::tanh(0.5f * (x + x1));
        else
            y = (fx - f1) / dx;
        x1 = x;
        f1 = fx;
        return y;
    }
};

} // namespace aurum::dsp
