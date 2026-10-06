#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dsp/ReverbEngine.h"

namespace aurum {

// Stable parameter identifiers (never renumber: hosts store them).
namespace pid {
enum : uint32_t
{
    Space = 1,
    DecayRate = 2,
    Style = 3,
    Predelay = 4,
    PredelaySync = 5,
    PredelayOffset = 6,
    Character = 7,
    Brightness = 8,
    Distance = 9,
    Thickness = 10,
    Ducking = 11,
    GateEnabled = 12,
    GateHold = 13,
    GateSync = 14,
    GateOffset = 15,
    Width = 16,
    Freeze = 17,
    Mix = 18,
    InputLevel = 19,
    InputPan = 20,
    OutputLevel = 21,
    OutputPan = 22,
    Bypass = 23,

    DecayBandBase = 100, // + band * 10 + field
    PostBandBase = 200,  // + band * 10 + field
};

enum DecayField : uint32_t { DUsed = 0, DEnabled, DShape, DFreq, DRate, DQ, DNumFields };
enum PostField : uint32_t { PUsed = 0, PEnabled, PShape, PFreq, PGain, PQ, PSlope, PPlacement, PNumFields };

constexpr uint32_t decay(int band, DecayField f) { return DecayBandBase + static_cast<uint32_t>(band) * 10 + f; }
constexpr uint32_t post(int band, PostField f) { return PostBandBase + static_cast<uint32_t>(band) * 10 + f; }
} // namespace pid

// How a parameter maps between its stored value and physical units.
enum class Unit : uint8_t
{
    Percent,      // 0..1 -> 0..100 %
    BipolarPercent, // 0..1 -> -100..+100 % (0.5 = neutral)
    DuckDb,       // 0..1 -> 0..24 dB of ducking
    WidthPercent, // 0..1 -> 0..150 %
    DecayRate,    // 0..1 -> 25..400 %
    OffsetPercent,// 0..1 -> 50..200 %
    PredelayMs,   // 0..1 -> 0..500 ms (piecewise linear taper)
    GateMs,       // 0..1 -> 10..2000 ms (log)
    Space,        // 0..1, shown as room name and decay time
    LevelDb,      // 0..1 -> -36..+36 dB
    Pan,          // 0..1 -> L100..R100
    FreqHz,       // 0..1 -> 10 Hz..30 kHz (log)
    RateLog2,     // 0..1 -> 12.5 %..800 % decay multiplier
    GainDb,       // 0..1 -> -30..+30 dB
    Q,            // 0..1 -> 0.025..40 (log)
    Enum,         // integer index
    Bool,
};

struct ParamDef
{
    uint32_t id;
    std::string key;    // stable text key for state/presets
    std::string name;   // display name
    std::string module; // CLAP module path
    Unit unit;
    double def;         // default (stored units)
    int steps = 0;      // number of enum entries (Enum/Bool)
    std::vector<std::string> labels;
    bool automatable = true;
    bool isBypass = false;
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

    std::string toText(int index, double value) const;
    std::optional<double> fromText(int index, const std::string& text) const;

private:
    ParamTable();
    std::vector<ParamDef> defs_;
    std::vector<int> byId_; // id -> index (or -1)
};

// Unit conversions (stored value <-> physical).
namespace conv {
double decayRate(double v);      // multiplier
double offset(double v);         // multiplier
double predelayMs(double v);
double predelayToValue(double ms);
double gateMs(double v);
double widthFrac(double v);      // 0..1.5
double levelDb(double v);
double pan(double v);            // -1..1
double freqHz(double v);
double rateLog2(double v);
double gainDb(double v);
double q(double v);

double freqToValue(double hz);
double qToValue(double q);
double rateLog2ToValue(double r);
double gainDbToValue(double db);
} // namespace conv

// Sync divisions for predelay / gate (index 0 = off).
constexpr int kNumSyncModes = 5;
double syncBeats(int mode); // length in quarter notes

// Convert a full set of stored values (indexed by ParamTable index) into
// engine parameters. `tempo` in BPM (<= 0 if unknown).
dsp::EngineParams buildEngineParams(const double* values, double tempo);

} // namespace aurum
