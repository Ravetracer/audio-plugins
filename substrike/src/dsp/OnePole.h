#pragma once

#include <cmath>

namespace substrike::dsp {

template <typename T> inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// One-pole lowpass and highpass, copied with the drive stage from the shared
// PluginCore library. The interface is float like the stage that uses them,
// but the state is double: at an oversampled rate a 5 Hz corner makes the
// coefficient so small that a float state stops moving about 1e-4 short of a
// constant input, and the offset that leaves never decays.
class OnePoleLp
{
public:
    void reset() { z_ = 0.0; }
    void setCutoff(float cutoffHz, float sampleRate)
    {
        const float f = clampv(cutoffHz / sampleRate, 1.0e-5f, 0.49f);
        coef_ = 1.0 - std::exp(-6.283185307179586 * f);
    }
    float tick(float in)
    {
        z_ += coef_ * (in - z_);
        return static_cast<float>(z_);
    }

private:
    double z_ = 0.0;
    double coef_ = 0.5;
};

class OnePoleHp
{
public:
    void reset() { z_ = 0.0; }
    void setCutoff(float cutoffHz, float sampleRate)
    {
        const float f = clampv(cutoffHz / sampleRate, 1.0e-5f, 0.49f);
        coef_ = 1.0 - std::exp(-6.283185307179586 * f);
    }
    float tick(float in)
    {
        z_ += coef_ * (in - z_);
        return static_cast<float>(in - z_);
    }

private:
    double z_ = 0.0;
    double coef_ = 0.01;
};

} // namespace substrike::dsp
