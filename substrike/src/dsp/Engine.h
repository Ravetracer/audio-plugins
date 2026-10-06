#pragma once

#include <array>

#include "dsp/Curve.h"

namespace substrike::dsp {

// Phase 1 has one lane; the parameter ids already leave room for eight.
constexpr int kNumLanes = 1;

// The tonal oscillator. Its pitch falls from pitchStart to pitchEnd along the
// pitch curve within sweepMs; its level runs attack, hold and then the amp
// curve within decayMs. Curvatures are -1..1 and bend the drawn curves.
struct BodyParams
{
    double pitchStart = 350.0; // Hz
    double pitchEnd = 48.0;    // Hz
    double sweepMs = 120.0;
    double sweepCurve = 0.55;
    double keyTrack = 0.0; // 0..1: share of the note's distance from the root
    double attackMs = 0.0;
    double holdMs = 30.0;
    double decayMs = 450.0;
    double decayCurve = 0.45;
    double phase = 0.0; // start phase in turns, latched per hit
};

struct LaneParams
{
    bool enabled = true;
    double gain = 1.0;     // linear
    double pan = 0.0;      // -1..1, balance law: unity in the centre
    double velocity = 0.5; // 0..1: how much velocity scales the level
    BodyParams body;
};

struct EngineParams
{
    std::array<LaneParams, kNumLanes> lanes{};
    double outGain = 1.0;
    int rootNote = 36;
};

class BodyVoice
{
public:
    void start(int semitones, double velocity, double phase);
    // Fades the voice out over `samples` (a retrigger or a choke).
    void fadeOut(int samples);
    bool active() const { return active_; }
    bool fading() const { return fadeStep_ > 0.0; }
    // Adds n samples into out.
    void render(float* out, int n, double sampleRate, const BodyParams& p, double velSense, const Curve& pitch,
                const Curve& amp);

private:
    bool active_ = false;
    double t_ = 0.0;     // samples since the hit
    double phase_ = 0.0; // turns
    double phase0_ = 0.0;
    double semitones_ = 0.0;
    double velocity_ = 1.0;
    double fade_ = 1.0;
    double fadeStep_ = 0.0;
};

class Lane
{
public:
    void reset();
    void noteOn(int semitones, double velocity, const LaneParams& p, int fadeSamples);
    void choke(int fadeSamples);
    bool active() const;
    // Adds n (<= kChunk) samples into left and right.
    void process(float* left, float* right, int n, double sampleRate, const LaneParams& p, double smoothCoef);

    Curve& pitchCurve() { return pitchCurve_; }
    Curve& ampCurve() { return ampCurve_; }

    static constexpr int kChunk = 256;

private:
    // One sounding hit, one fading out under it, and a spare for a third hit
    // inside the fade time.
    std::array<BodyVoice, 3> voices_{};
    Curve pitchCurve_;
    Curve ampCurve_;
    std::array<float, kChunk> mono_{};
    double gainL_ = 0.0, gainR_ = 0.0;
    bool primed_ = false;
};

class Engine
{
public:
    void prepare(double sampleRate);
    void reset();
    void noteOn(int key, double velocity, const EngineParams& p);
    void choke();
    // Writes (not adds) n samples.
    void process(float* left, float* right, int n, const EngineParams& p);
    bool idle() const;

private:
    double sampleRate_ = 48000.0;
    double smoothCoef_ = 0.0;
    int fadeSamples_ = 144;
    double outGain_ = 1.0;
    bool primed_ = false;
    std::array<Lane, kNumLanes> lanes_{};
};

} // namespace substrike::dsp
