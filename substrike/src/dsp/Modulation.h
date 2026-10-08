#pragma once

#include <cmath>
#include <cstdint>

#include "dsp/Curve.h"
#include "dsp/Random.h"

namespace substrike::dsp {

enum class LfoShape : int
{
    Sine = 0,
    Triangle,
    SawUp,
    SawDown,
    Square,
    SampleHold,
    Smooth,
};

// A low-frequency oscillator as a position in cycles: the phase is its
// fraction and the cycle its whole part, which S&H and Smooth draw a value
// for. The position is set from outside (free, retriggered or locked to the
// song) and the value read from it, so it is the same however the time
// between two reads was cut up.
class Lfo
{
public:
    explicit Lfo(uint64_t seed = 1) : seed_(seed) {}

    double position = 0.0;

    // -1..1, starting at 0 and rising for the periodic shapes.
    double value(LfoShape shape) const
    {
        constexpr double kTwoPi = 6.283185307179586;
        const double cycle = std::floor(position);
        const double p = position - cycle;
        switch (shape)
        {
        case LfoShape::Sine: return std::sin(kTwoPi * p);
        case LfoShape::Triangle: return p < 0.25 ? 4.0 * p : (p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0);
        case LfoShape::SawUp: return 2.0 * p - 1.0;
        case LfoShape::SawDown: return 1.0 - 2.0 * p;
        case LfoShape::Square: return p < 0.5 ? 1.0 : -1.0;
        case LfoShape::SampleHold: return random(cycle);
        case LfoShape::Smooth:
        {
            const double a = random(cycle), b = random(cycle + 1.0);
            return a + (b - a) * (0.5 - 0.5 * std::cos(3.141592653589793 * p));
        }
        }
        return 0.0;
    }

private:
    // One value per cycle, from the cycle's number alone.
    double random(double cycle) const
    {
        Rng r(seed_ ^ static_cast<uint64_t>(static_cast<int64_t>(cycle)) * 0x9E3779B97F4A7C15ull);
        return r.bipolar();
    }

    uint64_t seed_;
};

// A modulation envelope: a breakpoint curve run over a time from each note,
// once or looped. Before the first note it rests at the curve's end.
class ModEnvelope
{
public:
    void reset() { pos_ = -1.0; }
    void trigger() { pos_ = 0.0; }
    void advance(double samples)
    {
        if (pos_ >= 0.0)
            pos_ += samples;
    }

    double value(const Curve& curve, double lengthSamples, bool loop) const
    {
        if (pos_ < 0.0)
            return curve.eval(1.0, 0.0);
        double x = pos_ / std::max(lengthSamples, 1.0);
        if (loop)
            x -= std::floor(x);
        return curve.eval(x, 0.0);
    }

private:
    double pos_ = -1.0;
};

} // namespace substrike::dsp
