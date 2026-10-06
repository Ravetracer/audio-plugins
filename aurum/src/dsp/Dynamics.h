#pragma once

#include <cmath>

#include "Math.h"

namespace aurum::dsp {

// Peak envelope follower with separate attack and release.
struct EnvelopeFollower
{
    float env = 0.0f;
    float att = 0.0f, rel = 0.0f;
    void setTimes(double attackSec, double releaseSec, double fs)
    {
        att = onePoleCoeff(attackSec, fs);
        rel = onePoleCoeff(releaseSec, fs);
    }
    void reset() { env = 0.0f; }
    inline float process(float x)
    {
        const float c = x > env ? att : rel;
        env = x + (env - x) * c;
        return env;
    }
};

// Wet ducking keyed by the plugin input. The threshold adapts to the program:
// the input envelope is compared with its own slowly decaying peak, so the
// wet signal dips while the source plays and recovers in the gaps, regardless
// of the absolute input level.
class Ducker
{
public:
    void prepare(double fs)
    {
        fs_ = fs;
        env_.setTimes(0.002, 0.18, fs);
        gainSmooth_.setTimes(0.004, 0.12, fs);
        peakDecay_ = onePoleCoeff(2.5, fs);
        reset();
    }
    void reset()
    {
        env_.reset();
        gainSmooth_.reset();
        peak_ = 0.0f;
    }
    void setAmount(float amount) { amount_ = clamp(amount, 0.0f, 1.0f); }

    // Returns the wet gain for this sample.
    inline float process(float keyAbs)
    {
        const float e = env_.process(keyAbs);
        peak_ = std::max(e, peak_ * peakDecay_);
        float reductionDb = 0.0f;
        if (amount_ > 0.0f && peak_ > 1e-6f)
        {
            const float relDb = gainToDb(std::max(e, 1e-9f) / peak_); // <= 0
            const float depth = clamp((relDb + 30.0f) / 30.0f, 0.0f, 1.0f);
            reductionDb = depth * amount_ * kMaxRangeDb;
        }
        const float g = gainSmooth_.process(reductionDb);
        return dbToGain(-g);
    }

private:
    static constexpr float kMaxRangeDb = 24.0f;
    double fs_ = 48000.0;
    EnvelopeFollower env_;
    EnvelopeFollower gainSmooth_;
    float peak_ = 0.0f;
    float peakDecay_ = 0.0f;
    float amount_ = 0.0f;
};

// Transient-triggered gate for the wet signal. Onsets are found by comparing
// a fast and a slow envelope of the input; each onset (or sustained input)
// re-opens the gate for the hold time, after which the tail is faded out.
class AutoGate
{
public:
    void prepare(double fs)
    {
        fs_ = fs;
        fast_.setTimes(0.0005, 0.03, fs);
        slow_.setTimes(0.03, 0.4, fs);
        peakDecay_ = onePoleCoeff(3.0, fs);
        openStep_ = static_cast<float>(1.0 / (0.002 * fs));
        reset();
    }
    void reset()
    {
        fast_.reset();
        slow_.reset();
        peak_ = 0.0f;
        holdLeft_ = 0;
        gain_ = 1.0f;
    }
    void setEnabled(bool on) { enabled_ = on; }
    void setHold(double seconds)
    {
        hold_ = static_cast<int>(seconds * fs_);
        const double rel = clamp(seconds * 0.25, 0.02, 0.3);
        closeStep_ = static_cast<float>(1.0 / (rel * fs_));
    }

    inline float process(float keyAbs)
    {
        const float f = fast_.process(keyAbs);
        const float s = slow_.process(keyAbs);
        peak_ = std::max(f, peak_ * peakDecay_);
        if (!enabled_)
        {
            gain_ = std::min(1.0f, gain_ + openStep_);
            return gain_;
        }
        const bool onset = f > 1e-3f && f > s * 1.6f;
        const bool sustained = f > 1e-3f && f > peak_ * 0.35f;
        if (onset || sustained)
            holdLeft_ = hold_;
        if (holdLeft_ > 0)
        {
            --holdLeft_;
            gain_ = std::min(1.0f, gain_ + openStep_);
        }
        else
        {
            // Fade in the power domain for a natural-sounding close.
            gain_ = std::max(0.0f, gain_ - closeStep_);
        }
        return gain_ * gain_;
    }

private:
    double fs_ = 48000.0;
    EnvelopeFollower fast_, slow_;
    float peak_ = 0.0f, peakDecay_ = 0.0f;
    int hold_ = 0;
    int holdLeft_ = 0;
    float gain_ = 1.0f;
    float openStep_ = 0.0f, closeStep_ = 0.0f;
    bool enabled_ = false;
};

} // namespace aurum::dsp
