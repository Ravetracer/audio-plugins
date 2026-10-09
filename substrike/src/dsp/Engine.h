#pragma once

#include <array>
#include <cstdint>

#include "dsp/Crossover.h"
#include "dsp/Curve.h"
#include "dsp/Effects.h"
#include "dsp/Oversampler.h"
#include "dsp/Sources.h"

namespace substrike::dsp {

constexpr int kNumLanes = 8;
// Every lane has a pitch and an amplitude curve; a curve index is
// 2 * lane + which.
constexpr int kCurvesPerLane = 2;
constexpr int kNumLaneCurves = kNumLanes * kCurvesPerLane;
// After the lanes' curves come the modulation envelopes' (see the player).
constexpr int kNumModEnvCurves = 4;
constexpr int kNumCurves = kNumLaneCurves + kNumModEnvCurves;
constexpr int modEnvCurveIndex(int env) { return kNumLaneCurves + env; }
enum CurveKind : int
{
    PitchCurve = 0,
    AmpCurve = 1,
};
constexpr int curveIndex(int lane, int which) { return lane * kCurvesPerLane + which; }

enum class Source : int
{
    Body = 0,
    Click,
    Noise,
    Resonator,
    Bus,
};

enum class Output : int
{
    Main = 0,
    Aux,
    MainAndAux,
};

// A bus lane's input: the sum of the lanes switched on, each taken before or
// after its chain (after the chain means after its guard too, and before its
// level, pan and polarity). A lane never hears itself, and a bus that would
// close a loop hears silence from the lane that closes it.
struct BusParams
{
    std::array<bool, kNumLanes> from{};
    bool post = true;
};

// The transient guard at the end of a lane's chain. The window keeps the lane
// silent for `delayMs` after each of its hits and then fades it in over
// `fadeMs` (a raised cosine); both 0 switch it off. The duck lowers the lane
// while another lane sounds: by `duckDepth` at that lane's full scale, less
// as it falls, recovering over `duckReleaseMs`.
struct GuardParams
{
    double delayMs = 0.0, fadeMs = 0.0;
    int duckSource = -1; // -1: no duck
    double duckDepth = 1.0;
    double duckReleaseMs = 120.0;
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
    double chainKeyTrack = 0.0; // 0..1: how far the chain's tuned letters follow the note
    double variation = 0.0;  // 0..1: hit-to-hit randomness; 0 repeats every hit exactly
    BodyParams body;
    ClickParams click;
    NoiseParams noise;
    ResonatorParams resonator;
    BusParams bus;
    GuardParams guard;
    // The effect chain, and the two crossovers its band select splits at.
    std::array<SlotParams, kNumSlots> slots{};
    double xoverLow = 150.0, xoverHigh = 2500.0;
};

enum class OutputClip : int
{
    Off = 0,
    Soft,
    Hard,
    Limit, // a peak limiter at -0.3 dBFS rather than a clip
};

struct EngineParams
{
    std::array<LaneParams, kNumLanes> lanes{};
    double outGain = 1.0;
    int rootNote = 36;
    double tune = 0.0; // semitones, added to every lane's transpose
    int oversampling = 2; // 1, 2 or 4, for the drive group and the output clip
    std::array<SlotParams, kNumSlots> master{};
    double masterXoverLow = 150.0, masterXoverHigh = 2500.0;
    double monoBelow = 0.0; // Hz; 0 leaves the low end as it is
    OutputClip clip = OutputClip::Off;
    // Every lane also goes to its aux bus, whatever its Output says: the
    // editor's preview shows each lane on its own. Never set by the plugin.
    bool tapLanes = false;
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
        bool tap;
        double tempo;
        double tune; // semitones, the master's
        // A bus lane's input for this chunk (null for other sources), and the
        // offset from which it is silent.
        const float* busL;
        const float* busR;
        int busEnd;
    };
    // Adds n (<= kChunk) samples into the buses its Output names. Returns the
    // offset from which the lane is silent until the next chunk at least: n
    // while anything still sounds or rings in its chain.
    int process(const Bus& main, const Bus& aux, int n, const Context& c);
    // Prepares the chain for a new oversampling factor (and resets it).
    void prepareChain(double sampleRate, int oversampling);
    // Runs the duck's follower over n samples of the lane that drives it,
    // silent from `end` on. Called for every chunk, sounding or not, so the
    // follower does not depend on when this lane wakes.
    void follow(const float* l, const float* r, int end, int n, const LaneParams& p, double sampleRate);

    // The last chunk before and after the chain, for the bus lanes and the
    // ducks that read it, and the offsets from which each is silent.
    const float* preL() const { return preL_.data(); }
    const float* preR() const { return preR_.data(); }
    const float* postL() const { return l_.data(); }
    const float* postR() const { return r_.data(); }
    int preEnd() const { return preEnd_; }
    int postEnd() const { return postEnd_; }
    // The lane's output level as a peak follower (instant up, 50 ms down),
    // for the modulation; after the guard, before level and pan.
    double level() const { return level_; }

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
    // The window and the duck, in place on the chunk.
    void guard(int n, const Context& c);
    void renderVoices(int from, int to, const Context& c);
    bool voicesActive() const;

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
    bool snapGain_ = false;
    uint64_t hits_ = 0;

    std::array<Slot, kNumSlots> slots_{};
    // Offsets in this chunk at which hits fired, for the slots.
    std::array<int, kMaxPending> fired_{};
    std::array<double, kMaxPending> firedRatio_{};
    int firedCount_ = 0;
    // The first sample of this chunk after which no voice sounds.
    int soundEnd_ = 0;
    // Voices are done but the chain still rings; quiet_ counts the samples
    // it has been below the silence threshold since.
    bool tail_ = false;
    int quiet_ = 0;
    int quietLimit_ = 2400;

    std::array<float, kChunk> preL_{}, preR_{};
    int preEnd_ = 0, postEnd_ = 0;
    // The window's position since the last hit, in samples; open far out.
    int64_t window_ = INT64_MAX / 2;
    double duckEnv_ = 0.0;
    std::array<float, kChunk> duck_{};
    bool ducking_ = false;
    double level_ = 0.0;
    double levelFall_ = 0.9995;
};

class Engine
{
public:
    void prepare(double sampleRate);
    void reset();
    void noteOn(int key, double velocity, const EngineParams& p);
    void choke();
    // The host's tempo, for the synced delay times; 120 until one is known.
    void setTempo(double bpm) { tempo_ = bpm > 0.0 ? bpm : 120.0; }
    // Writes (not adds) n samples into every bus: buses[0] is the main output,
    // buses[1 + L] lane L's aux output. Every pointer must be valid.
    void process(const Bus* buses, int n, const EngineParams& p);
    bool idle() const;
    double laneLevel(int lane) const { return lanes_[static_cast<size_t>(lane)].level(); }
    // A lane's pitch or amplitude curve (curveIndex(), below kNumLaneCurves).
    // They are state, not parameters: the plugin sets them between process
    // calls.
    Curve& curve(int index)
    {
        Lane& l = lanes_[static_cast<size_t>(index / kCurvesPerLane)];
        return index % kCurvesPerLane == PitchCurve ? l.pitchCurve() : l.ampCurve();
    }

    static constexpr int kNumBuses = 1 + kNumLanes;

private:
    // The master chain, mono below and the output clip, in place on the main
    // bus. `lanesEnd` is where the lanes stopped sounding in this chunk.
    void master(const Bus& main, int n, int lanesEnd, const EngineParams& p);
    void resetMaster();
    void prepareLimit();

    double sampleRate_ = 48000.0;
    double tempo_ = 120.0;
    double smoothCoef_ = 0.0;
    int fadeSamples_ = 144;
    double outGain_ = 1.0;
    bool primed_ = false;
    bool snapOutput_ = false;
    int oversampling_ = 2;
    std::array<Lane, kNumLanes> lanes_{};
    std::array<float, Lane::kChunk> busL_{}, busR_{};

    std::array<Slot, kNumSlots> master_{};
    bool masterHit_ = false;
    bool masterTail_ = false;
    int masterQuiet_ = 0;
    int quietLimit_ = 2400;
    Crossover2 monoL_, monoR_;
    double monoFreq_ = 0.0;
    OutputClip clip_ = OutputClip::Off;
    LimiterFx outLimit_;
    Oversampler clipL_, clipR_;
    std::array<float, Lane::kChunk * Oversampler::kMaxFactor> clipBufL_{}, clipBufR_{};
};

} // namespace substrike::dsp
