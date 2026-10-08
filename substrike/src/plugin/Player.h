#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "dsp/Engine.h"
#include "dsp/Modulation.h"
#include "plugin/Params.h"

namespace substrike {

static_assert(pid::kNumModEnvs == dsp::kNumModEnvCurves, "one curve per modulation envelope");

// The engine with its modulation: the sources (velocity, note and a random
// value per note, the LFOs, the envelopes, the macros and the lanes'
// followers) and the matrix that adds them to parameters.
//
// Velocity, note and random are per lane: a route onto a lane's parameter
// reads the note that lane played last (its note filter decides which), and
// each lane draws its own random value per hit; a route onto a master or
// global parameter reads the last note of all.
//
// Modulation works on stored values, 0..1: a route adds Amount x its source
// to its destination's value, and the sum is clamped to the range, so a
// route's depth means the same on every parameter. The matrix is applied
// every kStep samples, counted from activation rather than from the block,
// and again at every note so a hit starts with its own velocity, note and
// random value; the engine's slots glide the values they get. With no route
// active the engine runs exactly as without a player.
class Player
{
public:
    static constexpr int kStep = 32;

    void prepare(double sampleRate);
    void reset();
    // The parameters changed: `values` is the full set, by table index.
    void setValues(const double* values);
    // The host's transport: its tempo, and its position in quarter notes if
    // it gives one (the synced LFOs lock to it).
    void setTransport(double tempo, bool hasBeats, double beats);
    void noteOn(int key, double velocity);
    void choke() { engine_.choke(); }
    // A new preset or state: what still sounds fades out over a few
    // milliseconds and the engine starts clean, so nothing of the old sound
    // (a delay's echoes, values gliding from the old settings) carries into
    // the new one. A note in the meantime plays when the fade is done.
    void freshStart();
    // As dsp::Engine::process.
    void process(const dsp::Bus* buses, int n);
    // The host's sample counter (steady time) at the start of a block. The
    // modulation's clock follows it, so the matrix's grid and the free LFOs
    // run on while the plugin sleeps. Without one, skip() counts the samples
    // of a block that was not processed.
    void setClock(uint64_t sample);
    void skip(int n) { advance(n); }
    bool idle() const { return engine_.idle(); }

    // Every curve: the lanes' (the engine's) and the envelopes'.
    dsp::Curve& curve(int index);
    // Sets a curve; during a fresh start's fade it waits for the fade's end
    // like the values do.
    void setCurve(int index, const dsp::Curve& c);
    int rootNote() const { return base_.rootNote; }
    // See EngineParams::tapLanes.
    void setTapLanes(bool tap);

    // The parameters the matrix moves (table indices) and where it has them
    // now, as stored values: for the editor's knobs.
    int routedParams(std::array<int, pid::kNumRoutes>& out) const
    {
        int n = 0;
        for (int r = 0; r < routeCount_; ++r)
        {
            const int d = routes_[static_cast<size_t>(r)].dest;
            bool seen = false;
            for (int i = 0; i < n; ++i)
                seen |= out[static_cast<size_t>(i)] == d;
            if (!seen)
                out[static_cast<size_t>(n++)] = d;
        }
        return n;
    }
    double modulatedValue(int index) const { return values_[static_cast<size_t>(index)]; }

    // A source's value right now, for the editor and the tests: `lane` for
    // the per-note sources as a lane sees them, -1 for the last note of all.
    double source(ModSource s, int lane = -1) const;

private:
    struct Route
    {
        ModSource source;
        int dest;  // table index
        int lane;  // the lane the destination belongs to, -1 for the master and global ones
        double amount; // -1..1
        double power;  // the curve, as an exponent on the source's size
    };
    struct LfoSetup
    {
        dsp::LfoShape shape = dsp::LfoShape::Sine;
        double rate = 4.0;  // Hz
        double beats = 0.0; // a synced length in quarter notes; 0 is free
        double phase = 0.0; // 0..1
        bool retrigger = true;
    };

    void modulate();
    void advance(int n);
    void render(const dsp::Bus* buses, int n);
    // Positions are worked out from whole sample counts since an anchor, so
    // they come out the same however the time was cut into blocks.
    double lfoPosition(int k) const;
    double beatsNow() const;
    void anchorLfo(int k, double position);
    void anchorBeats();

    // The fade of a fresh start, and the notes waiting for its end.
    static constexpr double kFreshFadeMs = 5.0;
    void finishFreshStart();
    int freshLeft_ = 0, freshLength_ = 1;
    // What arrives during the fade: the old sound fades out as it was, and
    // the new values and curves take over with the reset.
    std::vector<double> waitingValues_;
    bool valuesWaiting_ = false;
    std::array<dsp::Curve, dsp::kNumCurves> waitingCurves_{};
    std::array<bool, dsp::kNumCurves> curveWaiting_{};
    struct Deferred
    {
        int key;
        double velocity;
    };
    std::array<Deferred, 8> deferred_{};
    int deferredCount_ = 0;

    dsp::Engine engine_;
    double sampleRate_ = 48000.0;
    double tempo_ = 120.0;
    uint64_t clock_ = 0;

    std::vector<double> values_; // the stored values, modulated where routed
    std::vector<double> plain_;  // the same, as they came in
    dsp::EngineParams base_{};
    dsp::EngineParams params_{};
    bool tap_ = false;

    std::array<Route, pid::kNumRoutes> routes_{};
    int routeCount_ = 0;
    std::array<LfoSetup, pid::kNumLfos> lfoSetup_{};
    std::array<dsp::Lfo, pid::kNumLfos> lfos_{dsp::Lfo(0x51ull), dsp::Lfo(0x52ull), dsp::Lfo(0x53ull),
                                              dsp::Lfo(0x54ull)};
    std::array<uint64_t, pid::kNumLfos> lfoClock_{};
    std::array<double, pid::kNumLfos> lfoAnchor_{};
    std::array<double, pid::kNumModEnvs> envLength_{};
    std::array<bool, pid::kNumModEnvs> envLoop_{};
    std::array<dsp::ModEnvelope, pid::kNumModEnvs> envs_{};
    std::array<dsp::Curve, pid::kNumModEnvs> envCurves_{};
    std::array<double, pid::kNumMacros> macros_{};

    double velocity_ = 0.0, note_ = 0.0, random_ = 0.0;
    dsp::Rng rng_;
    std::array<double, dsp::kNumLanes> laneVelocity_{}, laneNote_{}, laneRandom_{};
    std::array<dsp::Rng, dsp::kNumLanes> laneRng_; // seeded by reset()
    // The song position: from the host when it gives one, else counted from
    // activation at the tempo.
    uint64_t beatsClock_ = 0;
    double beatsAnchor_ = 0.0;
};

} // namespace substrike
