#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "dsp/Engine.h"

namespace substrike {

// Stable parameter identifiers. Hosts store them, so an id is never renumbered
// or reused. They are assigned in blocks so every lane, and later every effect
// slot, has room to grow without touching its neighbours:
//
//   1 - 999        global
//   1000 * (L + 1) lane L (0-7): + 0-99 lane, + 100-199 Body, + 200-299
//                  Click, + 300-399 Noise, + 400-499 Resonator, + 500-599
//                  Bus, + 600-899 the six effect slots (50 each)
//   9000 - 9999    master chain
//   10000 -        modulation
namespace pid {
enum : uint32_t
{
    Output = 1,
    RootNote = 2,
    Quality = 3,
    Tune = 4,

    MasterXoverLow = 9300,
    MasterXoverHigh = 9301,
    MonoBelow = 9302,
    OutputClip = 9303,
};

enum LaneField : uint32_t
{
    LEnabled = 0,
    LLevel = 1,
    LPan = 2,
    LVelocity = 3,
    LSource = 4,
    LDelay = 5,
    LInvert = 6,
    LNote = 7,
    LOutput = 8,
    LPitchLink = 9,
    LTranspose = 10,
    LVariation = 11,
    LXoverLow = 12,
    LXoverHigh = 13,
    LGuardDelay = 14,
    LGuardFade = 15,
    LDuckSource = 16,
    LDuckDepth = 17,
    LDuckRelease = 18,
    LChainKeyTrack = 19,

    BodyPitchStart = 100,
    BodyPitchEnd = 101,
    BodySweep = 102,
    BodySweepCurve = 103,
    BodyKeyTrack = 104,
    BodyAttack = 110,
    BodyHold = 111,
    BodyDecay = 112,
    BodyDecayCurve = 113,
    BodyPhase = 120,
    BodyWave = 121,
    BodyShape = 122,
    BodyTilt = 123,
    BodyEven = 124,
    BodyStretch = 125,
    BodyFmAmount = 130,
    BodyFmRatio = 131,
    BodyFmDecay = 132,
    BodyFeedback = 133,
    BodyDrift = 140,

    ClickType = 200,
    ClickDecay = 201,
    ClickFilter = 202,
    ClickCutoff = 203,
    ClickReso = 204,
    ClickPitch = 205,
    ClickSweep = 206,

    NoiseColor = 300,
    NoiseDensity = 301,
    NoiseWidth = 302,
    NoiseFilter = 303,
    NoiseCutoff = 304,
    NoiseReso = 305,
    NoiseFilterEnv = 306,
    NoiseEnvDecay = 307,
    NoiseAttack = 308,
    NoiseHold = 309,
    NoiseDecay = 310,
    NoiseCurve = 311,

    ResExciter = 400,
    ResModel = 401,
    ResModes = 402,
    ResTune = 403,
    ResKeyTrack = 404,
    ResDecay = 405,
    ResDamping = 406,
    ResBrightness = 407,
    ResHardness = 408,
    ResDrop = 409,
    ResDropTime = 410,

    BusLane = 500, // .. 507: one switch per lane
    BusTap = 508,
};

constexpr uint32_t kLaneBlock = 1000;
constexpr uint32_t lane(int l, LaneField f) { return kLaneBlock * static_cast<uint32_t>(l + 1) + f; }

// An effect slot: 50 ids each, from lane offset 600 and from 9000 for the
// master. A-F are generic; the slot's type decides what they mean.
enum SlotField : uint32_t
{
    SType = 0,
    SBand = 1,
    SMix = 2,
    SBypass = 3,
    SValueA = 4, // .. SValueA + 5 for F
};
constexpr uint32_t kSlotBlock = 50;
constexpr uint32_t slot(int l, int s, uint32_t f)
{
    return lane(l, LEnabled) + 600 + kSlotBlock * static_cast<uint32_t>(s) + f;
}
constexpr uint32_t masterSlot(int s, uint32_t f) { return 9000 + kSlotBlock * static_cast<uint32_t>(s) + f; }

// Modulation. A route of the matrix: 10 ids each from 10000; an LFO: 20 each
// from 11000; an envelope: 20 each from 11200; the macros from 11400.
constexpr int kNumRoutes = 32;
constexpr int kNumLfos = 4;
constexpr int kNumModEnvs = 4;
constexpr int kNumMacros = 8;
enum RouteField : uint32_t
{
    RSource = 0,
    RDest = 1,
    RAmount = 2,
    RCurve = 3,
};
enum LfoField : uint32_t
{
    LfoShape = 0,
    LfoRate = 1,
    LfoSync = 2,
    LfoPhase = 3,
    LfoRetrigger = 4,
};
enum EnvField : uint32_t
{
    EnvTime = 0,
    EnvLoop = 1,
};
constexpr uint32_t route(int r, RouteField f) { return 10000 + 10 * static_cast<uint32_t>(r) + f; }
constexpr uint32_t lfo(int k, LfoField f) { return 11000 + 20 * static_cast<uint32_t>(k) + f; }
constexpr uint32_t modEnv(int k, EnvField f) { return 11200 + 20 * static_cast<uint32_t>(k) + f; }
constexpr uint32_t macro(int m) { return 11400 + static_cast<uint32_t>(m); }
} // namespace pid

// What a route reads, in the order its Source parameter lists them.
enum class ModSource : int
{
    Off = 0,
    Velocity,
    Note,
    Random,
    Lfo1,
    Env1 = Lfo1 + pid::kNumLfos,
    Macro1 = Env1 + pid::kNumModEnvs,
    Follow1 = Macro1 + pid::kNumMacros,
    Count = Follow1 + dsp::kNumLanes,
};

enum class Kind : uint8_t
{
    Continuous, // stored 0..1, mapped through `scale`
    Enum,       // stored as an index 0..labels-1
    Bool,
};

enum class Scale : uint8_t
{
    Linear,
    Log,   // lo * (hi / lo)^v, lo > 0
    Cubic, // lo + (hi - lo) * v^3: fine resolution near zero for times
};

struct ParamDef
{
    uint32_t id;
    std::string key;    // stable text key for state and presets
    std::string name;   // display name, unique
    std::string module; // CLAP module path
    Kind kind;
    Scale scale = Scale::Linear;
    double lo = 0.0, hi = 1.0; // plain range
    std::string unit;          // "Hz", "ms", "dB", "%", "deg", "pan", "st", "oct", ":1", "bit", ""
    double def = 0.0;          // default, stored (normalised) value
    std::vector<std::string> labels;
    // The lowest position means off: silence for a level, "Off" otherwise.
    bool floorIsOff = false;
    // A continuous value that is shown and set as one of these (a slot's
    // model or mode, which must stay continuous because the slot's type
    // decides whether it is a choice at all).
    std::vector<std::string> choices;
    bool automatable = true;
    // A slot's A-F: the table index of the slot's Type, and which letter.
    int typeParam = -1;
    int letter = -1;
    // A route's Destination: its labels are the destinations' names, one per
    // entry of ParamTable::destinations() after "Off".
    bool isDestination = false;
};

class ParamTable
{
public:
    static const ParamTable& get();

    int count() const { return static_cast<int>(defs_.size()); }
    const ParamDef& def(int index) const { return defs_[static_cast<size_t>(index)]; }
    int indexOf(uint32_t id) const;
    int indexOfKey(const std::string& key) const;

    double minValue(int index) const;
    double maxValue(int index) const;

    // What a parameter is right now. For everything but a slot's A-F that is
    // def(index); for those it is the definition the slot's type gives the
    // letter -- its name, key, range, unit and default. `typeValue` is the
    // stored value of the slot's Type, `values` the full set to look it up in.
    const ParamDef& effective(int index, double typeValue) const;
    const ParamDef& effective(int index, const double* values) const;

    // Stored value <-> plain value in the parameter's unit, and text.
    static double toPlain(const ParamDef& d, double value);
    static double fromPlain(const ParamDef& d, double plain);
    static std::string toText(const ParamDef& d, double value);
    static std::optional<double> fromText(const ParamDef& d, const std::string& text);
    // The same through def(index), for parameters that are not slot letters.
    double toPlain(int index, double value) const { return toPlain(def(index), value); }
    double fromPlain(int index, double plain) const { return fromPlain(def(index), plain); }
    std::string toText(int index, double value) const { return toText(def(index), value); }
    std::optional<double> fromText(int index, const std::string& text) const { return fromText(def(index), text); }

    // A slot letter's state key, resolved against the current types: the
    // letter it names, or -1.
    int indexOfSlotKey(const std::string& key, const double* values) const;
    // Whether this is a slot's Type, which the letters hang off.
    bool isSlotType(int index) const { return isType_[static_cast<size_t>(index)]; }

    // What a route can modulate: every continuous, automatable parameter
    // outside the modulation block, as table indices. A Destination's value
    // v > 0 means destinations()[v - 1].
    const std::vector<int>& destinations() const { return destinations_; }
    // The destination value for a parameter, 0 if it cannot be modulated.
    int destinationOf(int index) const { return destinationOf_[static_cast<size_t>(index)]; }

private:
    ParamTable();
    std::vector<ParamDef> defs_;
    std::vector<int> byId_; // id -> index (or -1)
    std::unordered_map<std::string, int> byKey_;
    // Per slot letter, its definition under every type.
    std::vector<int> view_; // index -> first entry in views_, or -1
    std::vector<bool> isType_;
    std::vector<ParamDef> views_;
    std::unordered_map<std::string, std::vector<int>> byViewKey_; // key -> indices into views_
    std::vector<int> destinations_;
    std::vector<int> destinationOf_;
};

std::string noteName(int midi);
bool isOff(const ParamDef& d, double plain);

// Convert a full set of stored values (indexed by ParamTable index) into
// engine parameters.
dsp::EngineParams buildEngineParams(const double* values);
// Writes the one parameter `index` into `p`, from `values` (a slot letter is
// read the way its slot's type defines it). Modulation parameters write
// nothing: the player reads those itself.
void assignParam(dsp::EngineParams& p, int index, const double* values);

} // namespace substrike
