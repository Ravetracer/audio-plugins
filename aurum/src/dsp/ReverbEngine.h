#pragma once

#include <array>
#include <memory>

#include "DecayDesigner.h"
#include "DecayModel.h"
#include "DelayLine.h"
#include "Dynamics.h"
#include "EarlyReflections.h"
#include "FdnTank.h"
#include "Math.h"
#include "PlateTank.h"
#include "PostEq.h"
#include "RoomModel.h"
#include "Svf.h"
#include "ClassicTank.h"

namespace aurum::dsp {

enum class Style : int { Natural = 0, Classic = 1, Plate = 2 };

// Plain (physical) parameter values consumed by the engine.
struct EngineParams
{
    double space = 0.5;      // 0..1
    double decayRate = 1.0;  // multiplier (0.25..4)
    Style style = Style::Natural;
    double predelayMs = 0.0; // 0..500
    double character = 0.25; // 0..1
    double brightness = 0.5; // 0..1
    double distance = 0.3;   // 0..1
    double thickness = 0.5;  // 0..1
    double ducking = 0.0;    // 0..1
    bool gateOn = false;
    double gateHoldMs = 250.0;
    double width = 1.0;      // 0..1.5
    bool freeze = false;
    double mix = 0.3;        // 0..1
    double inGainDb = 0.0, inPan = 0.0; // pan -1..1
    double outGainDb = 0.0, outPan = 0.0;
    bool bypass = false;
    std::array<DecayBand, kNumDecayBands> decayBands{};
    std::array<PostBand, kNumPostBands> postBands{};
};

// Derived control values (exposed for tests and the GUI).
struct EngineControl
{
    Room room;
    double theta = 0.785398;  // FDN mixing angle
    double modDepthMs = 0.0;
    double modRateHz = 0.3;
    double chorusMix = 0.0;
    double erLevelDb = 0.0;
    double erStartMs = 1.0, erLengthMs = 20.0, erSparsity = 0.3, erDiffusion = 0.4, erToneHz = 9000.0;
    double lateFeed = 1.0, erFeed = 0.0;
    double lateDiffusion = 0.6, lateDiffScale = 1.0;
    double satDrive = 1.0;
    double crossfeed = 0.0, sideGain = 1.0;
    // Brightness: a high and a low shelf on the wet signal.
    double toneHiHz = 5000.0, toneHiDb = 0.0, toneLoHz = 300.0, toneLoDb = 0.0;
    // Wet level from Brightness, Thickness and the room size.
    double wetLevelDb = 0.0;
    static EngineControl compute(const EngineParams& p, double space);
};

class ReverbEngine
{
public:
    ReverbEngine();
    ~ReverbEngine();

    void prepare(double sampleRate, int maxBlock);
    void release();
    void reset();
    void setOffline(bool offline) { offline_ = offline; }

    // Silences the reverb for a preset change: the wet signal fades out over
    // 10 ms on the settings it had, every tank and delay is cleared, and the
    // next block starts from the settings it is given with nothing gliding.
    // The dry path follows the new settings throughout, so it never jumps.
    // Audio thread, like process().
    void cut() { cutRequested_ = true; }

    // inL/inR may alias outL/outR.
    void process(const float* inL, const float* inR, float* outL, float* outR, int n, const EngineParams& p);

    // Longest audible tail in seconds for the given settings (< 0: infinite).
    static double tailSeconds(const EngineParams& p);

    static DecayModel decayModelFor(const EngineParams& p);
    static DecayModel decayModelFor(const EngineParams& p, const Room& room, Style style);


private:
    static constexpr int kBlock = 32;
    static constexpr int kTanks = 2;
    static constexpr int kLateDiffusers = 4;

    void processBlock(const float* inL, const float* inR, float* outL, float* outR, int n, const EngineParams& p);
    void updateControl(const EngineParams& p, bool snap);
    void maybeRequestDesign(const EngineParams& p, bool sync);
    int gatherSegments(double* delays) const;
    void applyCoeffs(const DecayCoeffSet& set);
    void switchStyle(Style s);
    void settle(const EngineParams& p);

    double fs_ = 48000.0;
    bool prepared_ = false;
    bool offline_ = false;
    Style style_ = Style::Natural;
    Style requestedStyle_ = Style::Natural;
    int styleFadeDir_ = 0; // -1 fading out for a switch, 0 steady
    float styleGain_ = 1.0f;
    float styleStep_ = 0.0f;
    int styleTag_ = 1;
    bool cutRequested_ = false;
    bool cutting_ = false;      // fading out for cut()
    EngineParams lastParams_{}; // what the previous process() call ran on
    EngineParams cutParams_{};  // the settings the fade-out runs on

    DecayDesigner designer_;
    std::unique_ptr<DecayCoeffSet> syncSet_;
    DecayModel lastModel_{};
    std::array<double, kMaxDecayLanes> lastDelays_{};
    int lastLanes_ = 0;
    bool lastFreeze_ = false;
    int lastTag_ = 0;
    int samplesSinceRequest_ = 1 << 20;
    EngineControl ctl_{};
    double space_ = 0.5;
    float spaceCoeff_ = 0.0f;

    std::array<FdnTank, kTanks> fdn_;
    std::array<ClassicTank, kTanks> classic_;
    std::array<PlateTank, kTanks> plate_;
    std::array<std::array<float, FdnTank::N>, kTanks> fdnPos_{};
    EarlyReflections er_;
    std::array<std::array<AllpassDiffuser, kLateDiffusers>, kTanks> lateDiff_;

    DelayLine predelayL_, predelayR_;
    float predelay_ = 0.0f, predelayTarget_ = 0.0f;
    float glide_ = 0.0f;
    float maxPredelay_ = 0.0f;

    AdaaTanh satL_, satR_;

    DelayLine chorusL_, chorusR_;
    std::array<OnePoleState, 2> chorusSplit_{};
    OnePoleCoeffs chorusSplitC_{};
    double chorusPhase_ = 0.0;

    SvfCoeffs toneHi_, toneLo_;

    // Per-style voicing, measured against reference renders.

    SvfCoeffs voiceHi_, voiceLo_;
    std::array<SvfState, 2> toneHiS_{}, toneLoS_{}, voiceHiS_{}, voiceLoS_{};

    PostEq postEq_;
    Ducker ducker_;
    AutoGate gate_;

    LinearSmoother inGainL_, inGainR_, outGainL_, outGainR_;
    LinearSmoother dryGain_, wetGain_, erGain_, lateFeed_, erFeed_;
    LinearSmoother inputMute_, sideGain_, crossfeed_, bypassMix_, chorusMix_, satDrive_;

};

} // namespace aurum::dsp
