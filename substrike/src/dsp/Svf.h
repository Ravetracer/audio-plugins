#pragma once

#include <algorithm>
#include <cmath>

namespace substrike::dsp {

enum class FilterMode : int
{
    Off = 0,
    LowPass,
    BandPass,
    HighPass,
};

// Trapezoidal state-variable filter (A. Simper, "Linear Trapezoidal Integrated
// State Variable Filter", Cytomic 2013). Stable under per-sample cutoff
// modulation, which the filter envelopes rely on.
class Svf
{
public:
    void reset() { ic1_ = ic2_ = 0.0; }

    void setup(double cutoff, double q, double sampleRate)
    {
        const double fc = std::clamp(cutoff, 10.0, 0.45 * sampleRate);
        const double g = std::tan(3.14159265358979323846 * fc / sampleRate);
        k_ = 1.0 / std::max(q, 0.05);
        a1_ = 1.0 / (1.0 + g * (g + k_));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }

    double process(double x, FilterMode mode)
    {
        const double v3 = x - ic2_;
        const double v1 = a1_ * ic1_ + a2_ * v3;
        const double v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0 * v1 - ic1_;
        ic2_ = 2.0 * v2 - ic2_;
        switch (mode)
        {
        case FilterMode::LowPass: return v2;
        case FilterMode::BandPass: return v1;
        case FilterMode::HighPass: return x - k_ * v1 - v2;
        case FilterMode::Off: break;
        }
        return x;
    }

private:
    double ic1_ = 0.0, ic2_ = 0.0;
    double k_ = 1.0, a1_ = 1.0, a2_ = 0.0, a3_ = 0.0;
};

// Resonance 0..1 onto Q 0.5..20, logarithmically.
inline double resonanceToQ(double r) { return 0.5 * std::pow(40.0, std::clamp(r, 0.0, 1.0)); }

} // namespace substrike::dsp
