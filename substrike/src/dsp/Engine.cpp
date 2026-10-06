#include "Engine.h"

#include <algorithm>
#include <cmath>

namespace substrike::dsp {

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
// A retrigger or a choke fades the old hit out over this long. Short enough
// not to smear the new transient, long enough not to click.
constexpr double kFadeMs = 3.0;
// Level, pan and output glide over this long, so automation does not zipper.
constexpr double kSmoothMs = 5.0;
} // namespace

// ------------------------------------------------------------------ BodyVoice

void BodyVoice::start(int semitones, double velocity, double phase)
{
    active_ = true;
    t_ = 0.0;
    phase_ = 0.0;
    phase0_ = phase - std::floor(phase);
    semitones_ = semitones;
    velocity_ = std::clamp(velocity, 0.0, 1.0);
    fade_ = 1.0;
    fadeStep_ = 0.0;
}

void BodyVoice::fadeOut(int samples)
{
    if (!active_ || fading())
        return;
    fadeStep_ = 1.0 / std::max(samples, 1);
}

void BodyVoice::render(float* out, int n, double sampleRate, const BodyParams& p, double velSense,
                       const Curve& pitch, const Curve& amp)
{
    if (!active_)
        return;
    const double msPerSample = 1000.0 / sampleRate;
    const double invRate = 1.0 / sampleRate;
    const double fEnd = p.pitchEnd * std::exp2(semitones_ * p.keyTrack / 12.0);
    const double logRatio = std::log(p.pitchStart / p.pitchEnd);
    const double fMax = 0.45 * sampleRate;
    const double sweep = std::max(p.sweepMs, 0.01);
    const double attack = std::max(p.attackMs, 0.0);
    const double holdEnd = attack + std::max(p.holdMs, 0.0);
    const double decay = std::max(p.decayMs, 0.1);
    const double level = 1.0 - std::clamp(velSense, 0.0, 1.0) * (1.0 - velocity_);

    for (int i = 0; i < n; ++i)
    {
        const double ms = t_ * msPerSample;
        double a;
        if (ms < attack)
            a = ms / attack;
        else if (ms < holdEnd)
            a = 1.0;
        else
        {
            const double x = (ms - holdEnd) / decay;
            if (x >= 1.0)
            {
                active_ = false;
                return;
            }
            a = amp.eval(x, p.decayCurve);
        }

        const double f = std::min(fEnd * std::exp(logRatio * pitch.eval(ms / sweep, p.sweepCurve)), fMax);
        out[i] += static_cast<float>(std::sin(kTwoPi * (phase_ + phase0_)) * a * level * fade_);

        phase_ += f * invRate;
        phase_ -= std::floor(phase_);
        t_ += 1.0;
        if (fadeStep_ > 0.0)
        {
            fade_ -= fadeStep_;
            if (fade_ <= 0.0)
            {
                active_ = false;
                return;
            }
        }
    }
}

// ----------------------------------------------------------------------- Lane

void Lane::reset()
{
    for (BodyVoice& v : voices_)
        v = BodyVoice{};
    primed_ = false;
}

void Lane::noteOn(int semitones, double velocity, const LaneParams& p, int fadeSamples)
{
    for (BodyVoice& v : voices_)
        v.fadeOut(fadeSamples);
    BodyVoice* target = nullptr;
    for (BodyVoice& v : voices_)
        if (!v.active())
        {
            target = &v;
            break;
        }
    // Three hits inside one fade time: cut the oldest fade short.
    if (!target)
        target = &voices_[0];
    target->start(semitones, velocity, p.body.phase);
}

void Lane::choke(int fadeSamples)
{
    for (BodyVoice& v : voices_)
        v.fadeOut(fadeSamples);
}

bool Lane::active() const
{
    for (const BodyVoice& v : voices_)
        if (v.active())
            return true;
    return false;
}

void Lane::process(float* left, float* right, int n, double sampleRate, const LaneParams& p, double smoothCoef)
{
    const double targetL = p.gain * std::min(1.0, 1.0 - p.pan);
    const double targetR = p.gain * std::min(1.0, 1.0 + p.pan);
    if (!primed_)
    {
        gainL_ = targetL;
        gainR_ = targetR;
        primed_ = true;
    }
    if (!active())
    {
        gainL_ = targetL;
        gainR_ = targetR;
        return;
    }
    std::fill(mono_.begin(), mono_.begin() + n, 0.0f);
    for (BodyVoice& v : voices_)
        v.render(mono_.data(), n, sampleRate, p.body, p.velocity, pitchCurve_, ampCurve_);
    for (int i = 0; i < n; ++i)
    {
        gainL_ += smoothCoef * (targetL - gainL_);
        gainR_ += smoothCoef * (targetR - gainR_);
        left[i] += static_cast<float>(mono_[static_cast<size_t>(i)] * gainL_);
        right[i] += static_cast<float>(mono_[static_cast<size_t>(i)] * gainR_);
    }
}

// --------------------------------------------------------------------- Engine

void Engine::prepare(double sampleRate)
{
    sampleRate_ = sampleRate;
    smoothCoef_ = 1.0 - std::exp(-1.0 / (kSmoothMs * 0.001 * sampleRate));
    fadeSamples_ = std::max(1, static_cast<int>(std::lround(kFadeMs * 0.001 * sampleRate)));
    reset();
}

void Engine::reset()
{
    for (Lane& l : lanes_)
        l.reset();
    primed_ = false;
}

void Engine::noteOn(int key, double velocity, const EngineParams& p)
{
    const int semitones = key - p.rootNote;
    for (int l = 0; l < kNumLanes; ++l)
    {
        const LaneParams& lp = p.lanes[static_cast<size_t>(l)];
        if (lp.enabled)
            lanes_[static_cast<size_t>(l)].noteOn(semitones, velocity, lp, fadeSamples_);
    }
}

void Engine::choke()
{
    for (Lane& l : lanes_)
        l.choke(fadeSamples_);
}

bool Engine::idle() const
{
    for (const Lane& l : lanes_)
        if (l.active())
            return false;
    return true;
}

void Engine::process(float* left, float* right, int n, const EngineParams& p)
{
    if (!primed_)
    {
        outGain_ = p.outGain;
        primed_ = true;
    }
    for (int pos = 0; pos < n; pos += Lane::kChunk)
    {
        const int m = std::min(Lane::kChunk, n - pos);
        float* l = left + pos;
        float* r = right + pos;
        std::fill(l, l + m, 0.0f);
        std::fill(r, r + m, 0.0f);
        for (int i = 0; i < kNumLanes; ++i)
        {
            const LaneParams& lp = p.lanes[static_cast<size_t>(i)];
            Lane& lane = lanes_[static_cast<size_t>(i)];
            // A lane switched off while it sounds fades out rather than
            // stopping dead.
            if (!lp.enabled)
                lane.choke(fadeSamples_);
            lane.process(l, r, m, sampleRate_, lp, smoothCoef_);
        }
        for (int i = 0; i < m; ++i)
        {
            outGain_ += smoothCoef_ * (p.outGain - outGain_);
            l[i] = static_cast<float>(l[i] * outGain_);
            r[i] = static_cast<float>(r[i] * outGain_);
        }
    }
}

} // namespace substrike::dsp
