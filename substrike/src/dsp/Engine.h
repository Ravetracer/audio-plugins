#pragma once

#include <array>
#include <cstdint>

#include "dsp/Curve.h"
#include "dsp/Sources.h"

namespace substrike::dsp {

constexpr int kNumLanes = 8;

enum class Source : int
{
    Body = 0,
    Click,
    Noise,
    Resonator,
};

enum class Output : int
{
    Main = 0,
    Aux,
    MainAndAux,
};

struct LaneParams
{
    bool enabled = true;
    double gain = 1.0;     // linear
    double pan = 0.0;      // -1..1, balance law: unity in the centre
    double velocity = 0.5; // 0..1: how much velocity scales the level
    Source source = Source::Body;
    double delayMs = 0.0; // the hit fires this much after the note
    bool invert = false;
    int note = -1; // -1: any note triggers the lane; else only this MIDI note
    Output output = Output::Main;
    int pitchLink = -1;      // -1: own pitch; else the lane whose Body pitch to follow
    double transpose = 0.0;  // semitones, every frequency of the source
    double variation = 0.0;  // 0..1: hit-to-hit randomness; 0 repeats every hit exactly
    BodyParams body;
    ClickParams click;
    NoiseParams noise;
    ResonatorParams resonator;
};

struct EngineParams
{
    std::array<LaneParams, kNumLanes> lanes{};
    double outGain = 1.0;
    int rootNote = 36;
};

// A stereo pair of output buffers.
struct Bus
{
    float* l = nullptr;
    float* r = nullptr;
};

class Lane
{
public:
    static constexpr int kChunk = 256;

    void prepare(double sampleRate);
    void reset();
    // Schedules a hit `delay` samples from the next sample processed.
    void trigger(int semitones, double velocity, int delay);
    // Fades every voice out and drops the scheduled hits.
    void choke(int fadeSamples);
    bool active() const;

    struct Context
    {
        double sampleRate;
        double smoothCoef;
        int fadeSamples;
        int index;
        const LaneParams* params;
        PitchTrack track;
    };
    // Adds n (<= kChunk) samples into the buses its Output names.
    void process(const Bus& main, const Bus& aux, int n, const Context& c);

    Curve& pitchCurve() { return pitchCurve_; }
    Curve& ampCurve() { return ampCurve_; }
    const Curve& pitchCurve() const { return pitchCurve_; }

private:
    struct Pending
    {
        int at; // samples from the start of the next chunk
        int semitones;
        double velocity;
    };

    void fire(const Pending& p, const Context& c);
    void renderVoices(int from, int to, const Context& c);

    static constexpr int kMaxPending = 8;
    std::array<Pending, kMaxPending> pending_{};
    int pendingCount_ = 0;

    // One sounding hit, one fading out under it, and a spare for a third hit
    // inside the fade time -- per source type, so switching the source lets
    // the old one fade too.
    std::array<BodyVoice, 3> body_{};
    std::array<ClickVoice, 3> click_{};
    std::array<NoiseVoice, 3> noise_{};
    std::array<ResonatorVoice, 3> resonator_{};

    Curve pitchCurve_;
    Curve ampCurve_;
    std::array<float, kChunk> l_{}, r_{};
    double gainL_ = 0.0, gainR_ = 0.0;
    bool primed_ = false;
    uint64_t hits_ = 0;
};

class Engine
{
public:
    void prepare(double sampleRate);
    void reset();
    void noteOn(int key, double velocity, const EngineParams& p);
    void choke();
    // Writes (not adds) n samples into every bus: buses[0] is the main output,
    // buses[1 + L] lane L's aux output. Every pointer must be valid.
    void process(const Bus* buses, int n, const EngineParams& p);
    bool idle() const;

    static constexpr int kNumBuses = 1 + kNumLanes;

private:
    double sampleRate_ = 48000.0;
    double smoothCoef_ = 0.0;
    int fadeSamples_ = 144;
    double outGain_ = 1.0;
    bool primed_ = false;
    std::array<Lane, kNumLanes> lanes_{};
};

} // namespace substrike::dsp
