#pragma once

#include <cstdint>
#include <optional>
#include <string>
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
//                  spare, + 600-899 the six effect slots (50 each)
//   9000 - 9999    master chain
//   10000 -        modulation
namespace pid {
enum : uint32_t
{
    Output = 1,
    RootNote = 2,
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
};

constexpr uint32_t kLaneBlock = 1000;
constexpr uint32_t lane(int l, LaneField f) { return kLaneBlock * static_cast<uint32_t>(l + 1) + f; }
} // namespace pid

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
    std::string unit;          // "Hz", "ms", "dB", "%", "deg", "pan", "st", "oct", "" -- drives text
    double def = 0.0;          // default, stored (normalised) value
    std::vector<std::string> labels;
    // A decibel parameter at its lowest position means silence.
    bool dbFloorIsOff = false;
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

    // Stored value <-> plain value in the parameter's unit.
    double toPlain(int index, double value) const;
    double fromPlain(int index, double plain) const;

    std::string toText(int index, double value) const;
    std::optional<double> fromText(int index, const std::string& text) const;

private:
    ParamTable();
    std::vector<ParamDef> defs_;
    std::vector<int> byId_; // id -> index (or -1)
};

std::string noteName(int midi);
bool isOff(const ParamDef& d, double plainDb);

// Convert a full set of stored values (indexed by ParamTable index) into
// engine parameters.
dsp::EngineParams buildEngineParams(const double* values);

} // namespace substrike
