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
    defs_.back().dbFloorIsOff = true;
    std::vector<std::string> notes;
    for (int n = 0; n < 128; ++n)
        notes.push_back(noteName(n));
    // MIDI 36, where a kick sits on most drum maps; named C2, with C4 = 60.
    defs_.push_back(enumeration(RootNote, "root_note", "Root Note", "Master", notes, 36));

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
            d.dbFloorIsOff = true;
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
    }

    uint32_t maxId = 0;
    for (const ParamDef& d : defs_)
        maxId = std::max(maxId, d.id);
    byId_.assign(maxId + 1, -1);
    for (int i = 0; i < count(); ++i)
    {
        ParamDef& d = defs_[static_cast<size_t>(i)];
        byId_[d.id] = i;
        if (d.kind == Kind::Continuous)
            d.def = fromPlain(i, d.def);
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
    for (int i = 0; i < count(); ++i)
        if (defs_[static_cast<size_t>(i)].key == key)
            return i;
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

double ParamTable::toPlain(int index, double v) const
{
    const ParamDef& d = def(index);
    if (d.kind != Kind::Continuous)
        return std::round(std::clamp(v, d.lo, d.hi));
    v = clamp01(v);
    switch (d.scale)
    {
    case Scale::Log: return d.lo * std::pow(d.hi / d.lo, v);
    case Scale::Cubic: return d.lo + (d.hi - d.lo) * v * v * v;
    case Scale::Linear: break;
    }
    return d.lo + (d.hi - d.lo) * v;
}

double ParamTable::fromPlain(int index, double plain) const
{
    const ParamDef& d = def(index);
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

std::string ParamTable::toText(int index, double value) const
{
    const ParamDef& d = def(index);
    if (d.kind != Kind::Continuous)
    {
        const int i = static_cast<int>(toPlain(index, value));
        return i >= 0 && i < static_cast<int>(d.labels.size()) ? d.labels[static_cast<size_t>(i)] : std::to_string(i);
    }
    const double x = toPlain(index, value);
    if (d.unit == "Hz")
        return x >= 999.5 ? sig3(x / 1000.0) + " kHz" : sig3(x) + " Hz";
    if (d.unit == "ms")
        return x >= 999.5 ? sig3(x / 1000.0) + " s" : sig3(x) + " ms";
    if (d.unit == "dB")
    {
        if (d.dbFloorIsOff && isOff(d, x))
            return "-inf dB";
        return fmt("%+.1f dB", shown(x, 0.1));
    }
    if (d.unit == "%")
        return fmt("%.0f %%", shown(x, 1.0));
    if (d.unit == "deg")
        return fmt("%.0f deg", shown(x, 1.0));
    if (d.unit == "st")
        return fmt("%+.1f st", shown(x, 0.1));
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

std::optional<double> ParamTable::fromText(int index, const std::string& text) const
{
    const ParamDef& d = def(index);
    const std::string t = trim(text);
    if (t.empty())
        return std::nullopt;

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
            return fromPlain(index, 440.0 * std::exp2((*midi - 69) / 12.0));
    if (d.unit == "pan")
    {
        if (lt == "c" || lt == "center")
            return fromPlain(index, 0.0);
        if (lt[0] == 'l' || lt[0] == 'r')
        {
            double v;
            std::string rest;
            if (!parseNumber(lt.substr(1), v, rest))
                return std::nullopt;
            return fromPlain(index, lt[0] == 'l' ? -v : v);
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
    return fromPlain(index, v);
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
        return t.def(i).dbFloorIsOff && isOff(t.def(i), db) ? 0.0 : std::pow(10.0, db / 20.0);
    };

    dsp::EngineParams p;
    p.outGain = gain(pid::Output);
    p.rootNote = static_cast<int>(plain(pid::RootNote));
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
    }
    return p;
}

} // namespace substrike
