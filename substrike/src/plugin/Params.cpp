#include "Params.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace substrike {

namespace {

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

std::string lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

// Parses a leading number; `rest` receives the remaining text (lower case,
// trimmed). Returns false if there is no number.
bool parseNumber(const std::string& text, double& value, std::string& rest)
{
    const std::string t = trim(text);
    char* end = nullptr;
    value = std::strtod(t.c_str(), &end);
    if (end == t.c_str())
        return false;
    rest = lower(trim(std::string(end)));
    return std::isfinite(value);
}

// Note names: "A4", "C#3", "Eb2", with C4 = MIDI 60. Returns a MIDI number.
std::optional<int> parseNoteNumber(const std::string& text)
{
    std::string t;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c)))
            t += c;
    if (t.size() < 2)
        return std::nullopt;
    static const int semis[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    const char n = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    if (n < 'A' || n > 'G')
        return std::nullopt;
    int semi = semis[n - 'A'];
    size_t i = 1;
    if (i < t.size() && (t[i] == '#' || t[i] == 'b'))
    {
        semi += t[i] == '#' ? 1 : -1;
        ++i;
    }
    char* end = nullptr;
    const long octave = std::strtol(t.c_str() + i, &end, 10);
    if (end == t.c_str() + i || *end != '\0')
        return std::nullopt;
    return static_cast<int>((octave + 1) * 12 + semi);
}

std::string fmt(const char* f, double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

// Rounds to the step a display shows and drops the sign of a zero, so -0.3 %
// prints as "0 %" rather than "-0 %", which would read back as +0 and print
// differently the second time.
double shown(double v, double step)
{
    double r = std::round(v / step) * step;
    if (r == 0.0)
        r = 0.0;
    return r;
}

// Three significant figures, which is what a knob can be set to anyway. The
// thresholds sit where the rounding carries over, so 99.97 prints as "100"
// and reads back to something that prints the same.
std::string sig3(double v)
{
    const double a = std::fabs(v);
    if (a >= 99.95)
        return fmt("%.0f", shown(v, 1.0));
    if (a >= 9.995)
        return fmt("%.1f", shown(v, 0.1));
    return fmt("%.2f", shown(v, 0.01));
}

ParamDef continuous(uint32_t id, std::string key, std::string name, std::string module, Scale scale, double lo,
                    double hi, std::string unit, double defPlain)
{
    ParamDef d;
    d.id = id;
    d.key = std::move(key);
    d.name = std::move(name);
    d.module = std::move(module);
    d.kind = Kind::Continuous;
    d.scale = scale;
    d.lo = lo;
    d.hi = hi;
    d.unit = std::move(unit);
    d.def = defPlain; // converted to a stored value once the table is built
    return d;
}

ParamDef enumeration(uint32_t id, std::string key, std::string name, std::string module,
                     std::vector<std::string> labels, int defIndex)
{
    ParamDef d;
    d.id = id;
    d.key = std::move(key);
    d.name = std::move(name);
    d.module = std::move(module);
    d.kind = Kind::Enum;
    d.labels = std::move(labels);
    d.lo = 0.0;
    d.hi = static_cast<double>(d.labels.size() - 1);
    d.def = defIndex;
    return d;
}

ParamDef boolean(uint32_t id, std::string key, std::string name, std::string module, bool def)
{
    ParamDef d = enumeration(id, std::move(key), std::move(name), std::move(module), {"Off", "On"}, def ? 1 : 0);
    d.kind = Kind::Bool;
    return d;
}

// What a slot's A-F mean under each type: the names, the state keys, the
// ranges and the defaults. The engine reads the values in these units; the
// effects in dsp/Effects.h say what they do with them. A letter a type does
// not use is left generic.
struct Shape
{
    const char* key;
    const char* name;
    Scale scale;
    double lo, hi;
    const char* unit;
    double def;
    std::vector<std::string> choices = {};
    bool floorIsOff = false;
};

const std::vector<std::string>& slotTypeLabels()
{
    static const std::vector<std::string> labels = {"Off",    "Distortion", "Clipper",    "Wavefolder", "Bitcrush",
                                                    "Filter", "EQ",         "Compressor", "Transient",  "Gate"};
    return labels;
}

const std::vector<std::vector<Shape>>& slotShapes()
{
    static const std::vector<std::vector<Shape>> shapes = {
        {}, // Off
        {
            {"model", "Model", Scale::Linear, 0, 9, "", 0,
             {"Soft Clip", "Overdrive", "Tube", "Valve Stack", "Fuzz", "Rectifier", "Crush", "Germanium", "Crunch",
              "Lead"}},
            {"drive", "Drive", Scale::Linear, 0, 100, "%", 40},
            {"bias", "Bias", Scale::Linear, -100, 100, "%", 0},
            {"tone", "Tone", Scale::Log, 200, 20000, "Hz", 20000},
            {"output", "Output", Scale::Linear, -24, 12, "dB", 0},
        },
        {
            {"drive", "Drive", Scale::Linear, 0, 36, "dB", 6},
            {"knee", "Knee", Scale::Linear, 0, 100, "%", 30},
            {"ceiling", "Ceiling", Scale::Linear, -24, 0, "dB", 0},
        },
        {
            {"drive", "Drive", Scale::Linear, 0, 36, "dB", 6},
            {"bias", "Bias", Scale::Linear, -100, 100, "%", 0},
            {"shape", "Shape", Scale::Linear, 0, 100, "%", 0},
            {"output", "Output", Scale::Linear, -24, 12, "dB", 0},
        },
        {
            {"bits", "Bits", Scale::Linear, 1, 16, "bit", 8},
            {"rate", "Rate", Scale::Log, 100, 48000, "Hz", 48000},
            {"output", "Output", Scale::Linear, -24, 12, "dB", 0},
        },
        {
            {"mode", "Mode", Scale::Linear, 0, 6, "", 1,
             {"LP 12", "LP 24", "HP 12", "HP 24", "Band Pass", "Notch", "Peak"}},
            {"cutoff", "Cutoff", Scale::Log, 20, 20000, "Hz", 2000},
            {"reso", "Reso", Scale::Linear, 0, 100, "%", 10},
            {"env", "Env", Scale::Linear, -8, 8, "oct", 0},
            {"env_decay", "Env Decay", Scale::Log, 5, 5000, "ms", 100},
            {"gain", "Gain", Scale::Linear, -24, 24, "dB", 0},
        },
        {
            {"low", "Low", Scale::Linear, -18, 18, "dB", 0},
            {"mid", "Mid", Scale::Linear, -18, 18, "dB", 0},
            {"mid_freq", "Mid Freq", Scale::Log, 30, 15000, "Hz", 1000},
            {"mid_q", "Mid Q", Scale::Log, 0.2, 10, "", 1},
            {"high", "High", Scale::Linear, -18, 18, "dB", 0},
            {"tilt", "Tilt", Scale::Linear, -12, 12, "dB", 0},
        },
        {
            {"threshold", "Threshold", Scale::Linear, -60, 0, "dB", -18},
            {"ratio", "Ratio", Scale::Log, 1, 20, ":1", 4},
            {"attack", "Attack", Scale::Log, 0.05, 200, "ms", 3},
            {"release", "Release", Scale::Log, 5, 2000, "ms", 80},
            {"knee", "Knee", Scale::Linear, 0, 24, "dB", 6},
            {"makeup", "Makeup", Scale::Linear, 0, 24, "dB", 0},
        },
        {
            {"attack", "Attack", Scale::Linear, -100, 100, "%", 0},
            {"sustain", "Sustain", Scale::Linear, -100, 100, "%", 0},
            {"speed", "Speed", Scale::Log, 1, 100, "ms", 20},
            {"output", "Output", Scale::Linear, -24, 12, "dB", 0},
        },
        {
            {"mode", "Mode", Scale::Linear, 0, 1, "", 0, {"Gate", "Hit"}},
            {"threshold", "Threshold", Scale::Linear, -80, 0, "dB", -40},
            {"attack", "Attack", Scale::Cubic, 0, 50, "ms", 0.5},
            {"hold", "Hold", Scale::Cubic, 0, 2000, "ms", 50},
            {"release", "Release", Scale::Log, 1, 5000, "ms", 100},
            {"range", "Range", Scale::Linear, -80, 0, "dB", -80, {}, true},
        },
    };
    return shapes;
}

} // namespace

// The bottom of a level range is silence. Anything that would display as the
// floor counts, so what the display says and what the engine does agree.
bool isOff(const ParamDef& d, double plainDb)
{
    return plainDb < d.lo + 0.05;
}

std::string noteName(int midi)
{
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    midi = std::clamp(midi, 0, 127);
    return std::string(names[midi % 12]) + std::to_string(midi / 12 - 1);
}

ParamTable::ParamTable()
{
    using namespace pid;
    defs_.push_back(continuous(Output, "output", "Output", "Master", Scale::Linear, -60.0, 12.0, "dB", -3.0));
    defs_.back().floorIsOff = true;
    std::vector<std::string> notes;
    for (int n = 0; n < 128; ++n)
        notes.push_back(noteName(n));
    // MIDI 36, where a kick sits on most drum maps; named C2, with C4 = 60.
    defs_.push_back(enumeration(RootNote, "root_note", "Root Note", "Master", notes, 36));
    // Oversampling for the drive slots and the output clip. Changing it resets
    // every chain, so it is a setting rather than something to automate.
    defs_.push_back(enumeration(Quality, "quality", "Quality", "Master", {"1x", "2x", "4x"}, 1));
    defs_.back().automatable = false;

    auto addSlot = [&](uint32_t base, const std::string& name, const std::string& key, const std::string& module) {
        const int typeIndex = static_cast<int>(defs_.size());
        defs_.push_back(enumeration(base + SType, key + "type", name + "Type", module, slotTypeLabels(), 0));
        // Picking a type rewrites what A-F mean, so it is not automated.
        defs_.back().automatable = false;
        defs_.push_back(enumeration(base + SBand, key + "band", name + "Band", module,
                                    {"Full", "Low", "Mid", "High", "Low+Mid", "Mid+High"}, 0));
        defs_.push_back(continuous(base + SMix, key + "mix", name + "Mix", module, Scale::Linear, 0.0, 100.0, "%",
                                   100.0));
        defs_.push_back(boolean(base + SBypass, key + "bypass", name + "Bypass", module, false));
        for (int k = 0; k < dsp::kSlotValues; ++k)
        {
            const char letter = static_cast<char>('A' + k);
            ParamDef d = continuous(base + SValueA + static_cast<uint32_t>(k),
                                    key + static_cast<char>(std::tolower(letter)), name + letter, module,
                                    Scale::Linear, 0.0, 100.0, "%", 50.0);
            d.typeParam = typeIndex;
            d.letter = k;
            defs_.push_back(std::move(d));
        }
    };

    std::vector<std::string> laneNotes{"Any"};
    for (int n = 0; n < 128; ++n)
        laneNotes.push_back(noteName(n));
    std::vector<std::string> links{"Own"};
    for (int l = 0; l < dsp::kNumLanes; ++l)
        links.push_back("Lane " + std::to_string(l + 1));
    const std::vector<std::string> filters{"Off", "Low Pass", "Band Pass", "High Pass"};

    for (int l = 0; l < dsp::kNumLanes; ++l)
    {
        const std::string n = std::to_string(l + 1);
        const std::string k = "l" + n + ".";
        const std::string p = "L" + n + " ";
        const std::string lane = "Lane " + n;
        const std::string body = lane + "/Body";
        const std::string click = lane + "/Click";
        const std::string noise = lane + "/Noise";
        const std::string res = lane + "/Resonator";
        auto add = [&](ParamDef d) { defs_.push_back(std::move(d)); };
        auto addDb = [&](ParamDef d) {
            d.floorIsOff = true;
            defs_.push_back(std::move(d));
        };

        // Lane 1 starts as the kick's body; lanes 2-4 are set up as a click, a
        // noise layer and a resonator, and switched off, so turning one on
        // gives something sensible at a sensible level.
        static const dsp::Source kSources[dsp::kNumLanes] = {dsp::Source::Body,  dsp::Source::Click,
                                                             dsp::Source::Noise, dsp::Source::Resonator,
                                                             dsp::Source::Body,  dsp::Source::Body,
                                                             dsp::Source::Body,  dsp::Source::Body};
        static const double kLevels[dsp::kNumLanes] = {0.0, -12.0, -18.0, -6.0, 0.0, 0.0, 0.0, 0.0};

        add(boolean(pid::lane(l, LEnabled), k + "on", p + "On", lane, l == 0));
        addDb(continuous(pid::lane(l, LLevel), k + "level", p + "Level", lane, Scale::Linear, -60.0, 12.0, "dB",
                         kLevels[l]));
        add(continuous(pid::lane(l, LPan), k + "pan", p + "Pan", lane, Scale::Linear, -100.0, 100.0, "pan", 0.0));
        add(continuous(pid::lane(l, LVelocity), k + "velocity", p + "Velocity", lane, Scale::Linear, 0.0, 100.0, "%",
                       50.0));
        add(enumeration(pid::lane(l, LSource), k + "source", p + "Source", lane,
                        {"Body", "Click", "Noise", "Resonator"}, static_cast<int>(kSources[l])));
        add(continuous(pid::lane(l, LDelay), k + "delay", p + "Delay", lane, Scale::Cubic, 0.0, 100.0, "ms", 0.0));
        add(boolean(pid::lane(l, LInvert), k + "invert", p + "Invert", lane, false));
        add(enumeration(pid::lane(l, LNote), k + "note", p + "Note", lane, laneNotes, 0));
        add(enumeration(pid::lane(l, LOutput), k + "output", p + "Output", lane, {"Main", "Aux", "Main+Aux"}, 0));
        add(enumeration(pid::lane(l, LPitchLink), k + "pitch_link", p + "Pitch Link", lane, links, 0));
        add(continuous(pid::lane(l, LTranspose), k + "transpose", p + "Transpose", lane, Scale::Linear, -24.0, 36.0,
                       "st", 0.0));
        add(continuous(pid::lane(l, LVariation), k + "variation", p + "Variation", lane, Scale::Linear, 0.0, 100.0,
                       "%", 0.0));
        // Where a slot's band select splits the lane.
        add(continuous(pid::lane(l, LXoverLow), k + "xover_low", p + "Crossover Low", lane, Scale::Log, 40.0, 1000.0,
                       "Hz", 150.0));
        add(continuous(pid::lane(l, LXoverHigh), k + "xover_high", p + "Crossover High", lane, Scale::Log, 500.0,
                       12000.0, "Hz", 2500.0));

        // The defaults are a plain, solid club kick: a fast drop from the low
        // mids onto a sub around G0, with a medium tail.
        add(continuous(pid::lane(l, BodyPitchStart), k + "body.pitch_start", p + "Pitch Start", body, Scale::Log,
                       20.0, 10000.0, "Hz", 350.0));
        add(continuous(pid::lane(l, BodyPitchEnd), k + "body.pitch_end", p + "Pitch End", body, Scale::Log, 20.0,
                       2000.0, "Hz", 48.0));
        add(continuous(pid::lane(l, BodySweep), k + "body.sweep", p + "Sweep Time", body, Scale::Log, 1.0, 2000.0,
                       "ms", 120.0));
        add(continuous(pid::lane(l, BodySweepCurve), k + "body.sweep_curve", p + "Sweep Curve", body, Scale::Linear,
                       -100.0, 100.0, "%", 55.0));
        add(continuous(pid::lane(l, BodyKeyTrack), k + "body.key_track", p + "Key Track", body, Scale::Linear, 0.0,
                       100.0, "%", 0.0));
        add(continuous(pid::lane(l, BodyAttack), k + "body.attack", p + "Body Attack", body, Scale::Cubic, 0.0, 100.0,
                       "ms", 0.0));
        add(continuous(pid::lane(l, BodyHold), k + "body.hold", p + "Body Hold", body, Scale::Cubic, 0.0, 2000.0, "ms",
                       30.0));
        add(continuous(pid::lane(l, BodyDecay), k + "body.decay", p + "Body Decay", body, Scale::Log, 5.0, 8000.0,
                       "ms", 450.0));
        add(continuous(pid::lane(l, BodyDecayCurve), k + "body.decay_curve", p + "Body Decay Curve", body,
                       Scale::Linear, -100.0, 100.0, "%", 45.0));
        add(continuous(pid::lane(l, BodyPhase), k + "body.phase", p + "Body Phase", body, Scale::Linear, 0.0, 360.0,
                       "deg", 0.0));
        add(enumeration(pid::lane(l, BodyWave), k + "body.wave", p + "Wave", body,
                        {"Sine", "Triangle", "Saw", "Square", "Additive"}, 0));
        add(continuous(pid::lane(l, BodyShape), k + "body.shape", p + "Shape", body, Scale::Linear, 0.0, 100.0, "%",
                       0.0));
        add(continuous(pid::lane(l, BodyTilt), k + "body.tilt", p + "Tilt", body, Scale::Linear, -100.0, 100.0, "%",
                       0.0));
        add(continuous(pid::lane(l, BodyEven), k + "body.even", p + "Even", body, Scale::Linear, 0.0, 100.0, "%",
                       100.0));
        add(continuous(pid::lane(l, BodyStretch), k + "body.stretch", p + "Stretch", body, Scale::Linear, 0.0, 100.0,
                       "%", 0.0));
        add(continuous(pid::lane(l, BodyFmAmount), k + "body.fm_amount", p + "FM Amount", body, Scale::Linear, 0.0,
                       100.0, "%", 0.0));
        add(continuous(pid::lane(l, BodyFmRatio), k + "body.fm_ratio", p + "FM Ratio", body, Scale::Log, 0.25, 16.0,
                       "", 2.0));
        add(continuous(pid::lane(l, BodyFmDecay), k + "body.fm_decay", p + "FM Decay", body, Scale::Log, 1.0, 2000.0,
                       "ms", 30.0));
        add(continuous(pid::lane(l, BodyFeedback), k + "body.feedback", p + "Feedback", body, Scale::Linear, 0.0,
                       100.0, "%", 0.0));
        add(continuous(pid::lane(l, BodyDrift), k + "body.drift", p + "Drift", body, Scale::Linear, 0.0, 100.0, "%",
                       0.0));

        add(enumeration(pid::lane(l, ClickType), k + "click.type", p + "Click Type", click,
                        {"Impulse", "Noise", "Blip", "Zap"}, 1));
        add(continuous(pid::lane(l, ClickDecay), k + "click.decay", p + "Click Decay", click, Scale::Log, 0.1, 100.0,
                       "ms", 6.0));
        add(enumeration(pid::lane(l, ClickFilter), k + "click.filter", p + "Click Filter", click, filters, 3));
        add(continuous(pid::lane(l, ClickCutoff), k + "click.cutoff", p + "Click Cutoff", click, Scale::Log, 20.0,
                       20000.0, "Hz", 2500.0));
        add(continuous(pid::lane(l, ClickReso), k + "click.reso", p + "Click Reso", click, Scale::Linear, 0.0, 100.0,
                       "%", 10.0));
        add(continuous(pid::lane(l, ClickPitch), k + "click.pitch", p + "Click Pitch", click, Scale::Log, 20.0,
                       10000.0, "Hz", 1500.0));
        add(continuous(pid::lane(l, ClickSweep), k + "click.sweep", p + "Click Sweep", click, Scale::Linear, 0.0, 8.0,
                       "oct", 3.0));

        add(enumeration(pid::lane(l, NoiseColor), k + "noise.color", p + "Noise Color", noise,
                        {"White", "Pink", "Brown", "Crackle"}, 0));
        add(continuous(pid::lane(l, NoiseDensity), k + "noise.density", p + "Noise Density", noise, Scale::Linear, 0.0,
                       100.0, "%", 50.0));
        add(continuous(pid::lane(l, NoiseWidth), k + "noise.width", p + "Noise Width", noise, Scale::Linear, 0.0, 100.0,
                       "%", 30.0));
        add(enumeration(pid::lane(l, NoiseFilter), k + "noise.filter", p + "Noise Filter", noise, filters, 2));
        add(continuous(pid::lane(l, NoiseCutoff), k + "noise.cutoff", p + "Noise Cutoff", noise, Scale::Log, 20.0,
                       20000.0, "Hz", 3000.0));
        add(continuous(pid::lane(l, NoiseReso), k + "noise.reso", p + "Noise Reso", noise, Scale::Linear, 0.0, 100.0,
                       "%", 20.0));
        add(continuous(pid::lane(l, NoiseFilterEnv), k + "noise.filter_env", p + "Noise Filter Env", noise,
                       Scale::Linear, -8.0, 8.0, "oct", 0.0));
        add(continuous(pid::lane(l, NoiseEnvDecay), k + "noise.env_decay", p + "Noise Env Decay", noise, Scale::Log,
                       1.0, 2000.0, "ms", 50.0));
        add(continuous(pid::lane(l, NoiseAttack), k + "noise.attack", p + "Noise Attack", noise, Scale::Cubic, 0.0,
                       100.0, "ms", 0.0));
        add(continuous(pid::lane(l, NoiseHold), k + "noise.hold", p + "Noise Hold", noise, Scale::Cubic, 0.0, 2000.0,
                       "ms", 0.0));
        add(continuous(pid::lane(l, NoiseDecay), k + "noise.decay", p + "Noise Decay", noise, Scale::Log, 5.0, 8000.0,
                       "ms", 120.0));
        add(continuous(pid::lane(l, NoiseCurve), k + "noise.curve", p + "Noise Curve", noise, Scale::Linear, -100.0,
                       100.0, "%", 50.0));

        add(enumeration(pid::lane(l, ResExciter), k + "resonator.exciter", p + "Resonator Exciter", res,
                        {"Impulse", "Mallet", "Noise"}, 1));
        add(enumeration(pid::lane(l, ResModel), k + "resonator.model", p + "Resonator Model", res,
                        {"Membrane", "Harmonic", "Odd", "Bar"}, 0));
        add(enumeration(pid::lane(l, ResModes), k + "resonator.modes", p + "Resonator Modes", res,
                        {"2", "3", "4", "5", "6", "7", "8"}, 4));
        add(continuous(pid::lane(l, ResTune), k + "resonator.tune", p + "Resonator Tune", res, Scale::Log, 20.0,
                       2000.0, "Hz", 80.0));
        add(continuous(pid::lane(l, ResKeyTrack), k + "resonator.key_track", p + "Resonator Key Track", res,
                       Scale::Linear, 0.0, 100.0, "%", 0.0));
        add(continuous(pid::lane(l, ResDecay), k + "resonator.decay", p + "Resonator Decay", res, Scale::Log, 10.0,
                       8000.0, "ms", 400.0));
        add(continuous(pid::lane(l, ResDamping), k + "resonator.damping", p + "Resonator Damping", res, Scale::Linear,
                       0.0, 100.0, "%", 50.0));
        add(continuous(pid::lane(l, ResBrightness), k + "resonator.brightness", p + "Resonator Brightness", res,
                       Scale::Linear, 0.0, 100.0, "%", 40.0));
        add(continuous(pid::lane(l, ResHardness), k + "resonator.hardness", p + "Resonator Hardness", res,
                       Scale::Linear, 0.0, 100.0, "%", 50.0));
        add(continuous(pid::lane(l, ResDrop), k + "resonator.drop", p + "Resonator Drop", res, Scale::Linear, 0.0,
                       24.0, "st", 0.0));
        add(continuous(pid::lane(l, ResDropTime), k + "resonator.drop_time", p + "Resonator Drop Time", res,
                       Scale::Log, 1.0, 1000.0, "ms", 30.0));

        for (int s = 0; s < dsp::kNumSlots; ++s)
        {
            const std::string sn = std::to_string(s + 1);
            addSlot(pid::slot(l, s, 0), p + "Slot " + sn + " ", k + "slot" + sn + ".", lane + "/Slot " + sn);
        }
    }

    for (int s = 0; s < dsp::kNumSlots; ++s)
    {
        const std::string sn = std::to_string(s + 1);
        addSlot(pid::masterSlot(s, 0), "Master Slot " + sn + " ", "master.slot" + sn + ".", "Master/Slot " + sn);
    }
    defs_.push_back(continuous(MasterXoverLow, "master.xover_low", "Master Crossover Low", "Master", Scale::Log, 40.0,
                               1000.0, "Hz", 150.0));
    defs_.push_back(continuous(MasterXoverHigh, "master.xover_high", "Master Crossover High", "Master", Scale::Log,
                               500.0, 12000.0, "Hz", 2500.0));
    // Below this the output is mono. The bottom of the range leaves it alone.
    defs_.push_back(continuous(MonoBelow, "master.mono_below", "Mono Below", "Master", Scale::Log, 20.0, 500.0, "Hz",
                               20.0));
    defs_.back().floorIsOff = true;
    defs_.push_back(enumeration(OutputClip, "master.clip", "Output Clip", "Master", {"Off", "Soft", "Hard"}, 0));

    uint32_t maxId = 0;
    for (const ParamDef& d : defs_)
        maxId = std::max(maxId, d.id);
    byId_.assign(maxId + 1, -1);
    for (int i = 0; i < count(); ++i)
    {
        ParamDef& d = defs_[static_cast<size_t>(i)];
        byId_[d.id] = i;
        byKey_[d.key] = i;
        if (d.kind == Kind::Continuous)
            d.def = fromPlain(i, d.def);
    }

    // Every slot letter under every type.
    view_.assign(defs_.size(), -1);
    isType_.assign(defs_.size(), false);
    for (int i = 0; i < count(); ++i)
    {
        const ParamDef& generic = defs_[static_cast<size_t>(i)];
        if (generic.typeParam < 0)
            continue;
        isType_[static_cast<size_t>(generic.typeParam)] = true;
        const std::string& name = generic.name;
        const std::string namePrefix = name.substr(0, name.size() - 1);
        const std::string keyPrefix = generic.key.substr(0, generic.key.size() - 1);
        view_[static_cast<size_t>(i)] = static_cast<int>(views_.size());
        for (int t = 0; t < dsp::kNumSlotTypes; ++t)
        {
            ParamDef v = generic;
            const std::vector<Shape>& shape = slotShapes()[static_cast<size_t>(t)];
            if (generic.letter < static_cast<int>(shape.size()))
            {
                const Shape& s = shape[static_cast<size_t>(generic.letter)];
                v.name = namePrefix + s.name;
                v.key = keyPrefix + s.key;
                v.scale = s.scale;
                v.lo = s.lo;
                v.hi = s.hi;
                v.unit = s.unit;
                v.choices = s.choices;
                v.floorIsOff = s.floorIsOff;
                v.def = fromPlain(v, s.def);
            }
            byViewKey_[v.key].push_back(static_cast<int>(views_.size()));
            views_.push_back(std::move(v));
        }
    }
}

const ParamTable& ParamTable::get()
{
    static const ParamTable table;
    return table;
}

int ParamTable::indexOf(uint32_t id) const
{
    return id < byId_.size() ? byId_[id] : -1;
}

int ParamTable::indexOfKey(const std::string& key) const
{
    const auto it = byKey_.find(key);
    return it == byKey_.end() ? -1 : it->second;
}

const ParamDef& ParamTable::effective(int index, double typeValue) const
{
    const int v = view_[static_cast<size_t>(index)];
    if (v < 0)
        return def(index);
    const int type = std::clamp(static_cast<int>(std::lround(typeValue)), 0, dsp::kNumSlotTypes - 1);
    return views_[static_cast<size_t>(v + type)];
}

const ParamDef& ParamTable::effective(int index, const double* values) const
{
    const int t = def(index).typeParam;
    return t < 0 ? def(index) : effective(index, values[t]);
}

int ParamTable::indexOfSlotKey(const std::string& key, const double* values) const
{
    const auto it = byViewKey_.find(key);
    if (it == byViewKey_.end())
        return -1;
    for (int v : it->second)
    {
        const ParamDef& d = views_[static_cast<size_t>(v)];
        const int index = indexOf(d.id);
        if (&effective(index, values) == &d)
            return index;
    }
    return -1;
}

double ParamTable::minValue(int index) const
{
    return def(index).kind == Kind::Continuous ? 0.0 : def(index).lo;
}

double ParamTable::maxValue(int index) const
{
    return def(index).kind == Kind::Continuous ? 1.0 : def(index).hi;
}

double ParamTable::toPlain(const ParamDef& d, double v)
{
    if (d.kind != Kind::Continuous)
        return std::round(std::clamp(v, d.lo, d.hi));
    v = clamp01(v);
    if (!d.choices.empty())
        return std::round(d.lo + (d.hi - d.lo) * v);
    switch (d.scale)
    {
    case Scale::Log: return d.lo * std::pow(d.hi / d.lo, v);
    case Scale::Cubic: return d.lo + (d.hi - d.lo) * v * v * v;
    case Scale::Linear: break;
    }
    return d.lo + (d.hi - d.lo) * v;
}

double ParamTable::fromPlain(const ParamDef& d, double plain)
{
    if (d.kind != Kind::Continuous)
        return std::round(std::clamp(plain, d.lo, d.hi));
    plain = std::clamp(plain, d.lo, d.hi);
    switch (d.scale)
    {
    case Scale::Log: return clamp01(std::log(plain / d.lo) / std::log(d.hi / d.lo));
    case Scale::Cubic: return clamp01(std::cbrt((plain - d.lo) / (d.hi - d.lo)));
    case Scale::Linear: break;
    }
    return clamp01((plain - d.lo) / (d.hi - d.lo));
}

std::string ParamTable::toText(const ParamDef& d, double value)
{
    const std::vector<std::string>& names = d.kind != Kind::Continuous ? d.labels : d.choices;
    if (!names.empty())
    {
        const int i = static_cast<int>(toPlain(d, value) - (d.kind == Kind::Continuous ? d.lo : 0.0));
        return i >= 0 && i < static_cast<int>(names.size()) ? names[static_cast<size_t>(i)] : std::to_string(i);
    }
    const double x = toPlain(d, value);
    if (d.floorIsOff && d.unit != "dB" && isOff(d, x))
        return "Off";
    if (d.unit == "Hz")
        return x >= 999.5 ? sig3(x / 1000.0) + " kHz" : sig3(x) + " Hz";
    if (d.unit == "ms")
        return x >= 999.5 ? sig3(x / 1000.0) + " s" : sig3(x) + " ms";
    if (d.unit == "dB")
    {
        if (d.floorIsOff && isOff(d, x))
            return "-inf dB";
        return fmt("%+.1f dB", shown(x, 0.1));
    }
    if (d.unit == "%")
        return fmt("%.0f %%", shown(x, 1.0));
    if (d.unit == "deg")
        return fmt("%.0f deg", shown(x, 1.0));
    if (d.unit == "st")
        return fmt("%+.1f st", shown(x, 0.1));
    if (d.unit == ":1")
        return sig3(x) + ":1";
    if (d.unit == "bit")
        return fmt("%.1f bit", shown(x, 0.1));
    if (d.unit == "oct")
        return fmt("%+.1f oct", shown(x, 0.1));
    if (d.unit == "pan")
    {
        const long r = std::lround(x);
        if (r == 0)
            return "C";
        return (r < 0 ? "L" : "R") + std::to_string(std::labs(r));
    }
    return sig3(x);
}

std::optional<double> ParamTable::fromText(const ParamDef& d, const std::string& text)
{
    const std::string t = trim(text);
    if (t.empty())
        return std::nullopt;

    if (d.kind == Kind::Continuous && !d.choices.empty())
    {
        const std::string lt = lower(t);
        for (size_t i = 0; i < d.choices.size(); ++i)
            if (lower(d.choices[i]) == lt)
                return fromPlain(d, d.lo + static_cast<double>(i));
        return std::nullopt;
    }
    if (d.floorIsOff && d.unit != "dB" && lower(t) == "off")
        return 0.0;

    if (d.kind != Kind::Continuous)
    {
        const std::string lt = lower(t);
        for (size_t i = 0; i < d.labels.size(); ++i)
            if (lower(d.labels[i]) == lt)
                return static_cast<double>(i);
        if (d.kind == Kind::Bool)
        {
            if (lt == "true" || lt == "yes")
                return 1.0;
            if (lt == "false" || lt == "no")
                return 0.0;
        }
        double v;
        std::string rest;
        if (parseNumber(t, v, rest) && rest.empty())
            return std::clamp(std::round(v), d.lo, d.hi);
        return std::nullopt;
    }

    const std::string lt = lower(t);
    if (d.unit == "Hz")
        if (auto midi = parseNoteNumber(t))
            return fromPlain(d, 440.0 * std::exp2((*midi - 69) / 12.0));
    if (d.unit == "pan")
    {
        if (lt == "c" || lt == "center")
            return fromPlain(d, 0.0);
        if (lt[0] == 'l' || lt[0] == 'r')
        {
            double v;
            std::string rest;
            if (!parseNumber(lt.substr(1), v, rest))
                return std::nullopt;
            return fromPlain(d, lt[0] == 'l' ? -v : v);
        }
    }
    if (d.unit == "dB" && (lt == "-inf" || lt == "-inf db" || lt == "off"))
        return 0.0;

    double v;
    std::string rest;
    if (!parseNumber(t, v, rest))
        return std::nullopt;
    if (d.unit == "Hz" && !rest.empty() && rest[0] == 'k')
        v *= 1000.0;
    if (d.unit == "ms" && (rest == "s" || rest == "sec"))
        v *= 1000.0;
    return fromPlain(d, v);
}

dsp::EngineParams buildEngineParams(const double* values)
{
    const ParamTable& t = ParamTable::get();
    auto plain = [&](uint32_t id) {
        const int i = t.indexOf(id);
        return t.toPlain(i, values[i]);
    };
    auto gain = [&](uint32_t id) {
        const int i = t.indexOf(id);
        const double db = t.toPlain(i, values[i]);
        return t.def(i).floorIsOff && isOff(t.def(i), db) ? 0.0 : std::pow(10.0, db / 20.0);
    };

    auto slot = [&](uint32_t base, dsp::SlotParams& s) {
        using namespace pid;
        s.type = static_cast<dsp::SlotType>(static_cast<int>(plain(base + SType)));
        s.band = static_cast<dsp::Band>(static_cast<int>(plain(base + SBand)));
        s.mix = plain(base + SMix) / 100.0;
        s.bypass = plain(base + SBypass) > 0.5;
        for (int k = 0; k < dsp::kSlotValues; ++k)
        {
            const int i = t.indexOf(base + SValueA + static_cast<uint32_t>(k));
            s.v[static_cast<size_t>(k)] = ParamTable::toPlain(t.effective(i, values), values[i]);
        }
    };

    dsp::EngineParams p;
    p.outGain = gain(pid::Output);
    p.rootNote = static_cast<int>(plain(pid::RootNote));
    p.oversampling = 1 << static_cast<int>(plain(pid::Quality));
    for (int s = 0; s < dsp::kNumSlots; ++s)
        slot(pid::masterSlot(s, 0), p.master[static_cast<size_t>(s)]);
    p.masterXoverLow = plain(pid::MasterXoverLow);
    p.masterXoverHigh = plain(pid::MasterXoverHigh);
    {
        const int i = t.indexOf(pid::MonoBelow);
        const double hz = t.toPlain(i, values[i]);
        p.monoBelow = isOff(t.def(i), hz) ? 0.0 : hz;
    }
    p.clip = static_cast<dsp::OutputClip>(static_cast<int>(plain(pid::OutputClip)));
    for (int l = 0; l < dsp::kNumLanes; ++l)
    {
        using namespace pid;
        auto lv = [&](LaneField f) { return plain(lane(l, f)); };
        auto pct = [&](LaneField f) { return plain(lane(l, f)) / 100.0; };
        auto idx = [&](LaneField f) { return static_cast<int>(plain(lane(l, f))); };

        dsp::LaneParams& lp = p.lanes[static_cast<size_t>(l)];
        lp.enabled = lv(LEnabled) > 0.5;
        lp.gain = gain(lane(l, LLevel));
        lp.pan = pct(LPan);
        lp.velocity = pct(LVelocity);
        lp.source = static_cast<dsp::Source>(idx(LSource));
        lp.delayMs = lv(LDelay);
        lp.invert = lv(LInvert) > 0.5;
        lp.note = idx(LNote) - 1;
        lp.output = static_cast<dsp::Output>(idx(LOutput));
        lp.pitchLink = idx(LPitchLink) - 1;
        lp.transpose = lv(LTranspose);
        lp.variation = pct(LVariation);

        dsp::BodyParams& b = lp.body;
        b.pitchStart = lv(BodyPitchStart);
        b.pitchEnd = lv(BodyPitchEnd);
        b.sweepMs = lv(BodySweep);
        b.sweepCurve = pct(BodySweepCurve);
        b.keyTrack = pct(BodyKeyTrack);
        b.attackMs = lv(BodyAttack);
        b.holdMs = lv(BodyHold);
        b.decayMs = lv(BodyDecay);
        b.decayCurve = pct(BodyDecayCurve);
        b.phase = lv(BodyPhase) / 360.0;
        b.wave = static_cast<dsp::Wave>(idx(BodyWave));
        b.shape = pct(BodyShape);
        b.tilt = pct(BodyTilt);
        b.even = pct(BodyEven);
        b.stretch = pct(BodyStretch);
        b.fmAmount = pct(BodyFmAmount);
        b.fmRatio = lv(BodyFmRatio);
        b.fmDecayMs = lv(BodyFmDecay);
        b.feedback = pct(BodyFeedback);
        b.drift = pct(BodyDrift);

        dsp::ClickParams& c = lp.click;
        c.type = static_cast<dsp::ClickType>(idx(ClickType));
        c.decayMs = lv(ClickDecay);
        c.filter = static_cast<dsp::FilterMode>(idx(ClickFilter));
        c.cutoff = lv(ClickCutoff);
        c.reso = pct(ClickReso);
        c.pitch = lv(ClickPitch);
        c.sweepOct = lv(ClickSweep);

        dsp::NoiseParams& nz = lp.noise;
        nz.color = static_cast<dsp::NoiseColor>(idx(NoiseColor));
        nz.density = pct(NoiseDensity);
        nz.width = pct(NoiseWidth);
        nz.filter = static_cast<dsp::FilterMode>(idx(NoiseFilter));
        nz.cutoff = lv(NoiseCutoff);
        nz.reso = pct(NoiseReso);
        nz.filterEnvOct = lv(NoiseFilterEnv);
        nz.envDecayMs = lv(NoiseEnvDecay);
        nz.attackMs = lv(NoiseAttack);
        nz.holdMs = lv(NoiseHold);
        nz.decayMs = lv(NoiseDecay);
        nz.curve = pct(NoiseCurve);

        dsp::ResonatorParams& r = lp.resonator;
        r.exciter = static_cast<dsp::Exciter>(idx(ResExciter));
        r.model = static_cast<dsp::ResonatorModel>(idx(ResModel));
        r.modes = idx(ResModes) + 2;
        r.tune = lv(ResTune);
        r.keyTrack = pct(ResKeyTrack);
        r.decayMs = lv(ResDecay);
        r.damping = pct(ResDamping);
        r.brightness = pct(ResBrightness);
        r.hardness = pct(ResHardness);
        r.dropSt = lv(ResDrop);
        r.dropMs = lv(ResDropTime);

        lp.xoverLow = lv(LXoverLow);
        lp.xoverHigh = lv(LXoverHigh);
        for (int s = 0; s < dsp::kNumSlots; ++s)
            slot(pid::slot(l, s, 0), lp.slots[static_cast<size_t>(s)]);
    }
    return p;
}

} // namespace substrike
