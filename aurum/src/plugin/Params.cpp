#include "Params.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "dsp/RoomModel.h"

namespace aurum {

namespace conv {
double decayRate(double v) { return std::exp2(-2.0 + 4.0 * v); }
double offset(double v) { return std::exp2(-1.0 + 2.0 * v); }
// Predelay taper: piecewise linear between knob positions 0.0, 0.1, ... 1.0
// (fine resolution for short times; imported .ffp values map 1:1).
constexpr std::array<double, 11> kPredelayAnchorsMs = {0.0, 1.0, 5.0, 15.0, 30.0, 50.0, 90.0, 140.0, 200.0, 300.0, 500.0};
double predelayMs(double v)
{
    const double pos = dsp::clamp(v, 0.0, 1.0) * 10.0;
    const int i = std::min(static_cast<int>(pos), 9);
    return dsp::lerp(kPredelayAnchorsMs[i], kPredelayAnchorsMs[i + 1], pos - i);
}
double predelayToValue(double ms)
{
    ms = dsp::clamp(ms, 0.0, kPredelayAnchorsMs.back());
    int i = 0;
    while (i < 9 && ms > kPredelayAnchorsMs[i + 1])
        ++i;
    return (i + (ms - kPredelayAnchorsMs[i]) / (kPredelayAnchorsMs[i + 1] - kPredelayAnchorsMs[i])) / 10.0;
}
double gateMs(double v) { return 10.0 * std::pow(200.0, v); }
double widthFrac(double v) { return 1.5 * v; }
double levelDb(double v) { return -36.0 + 72.0 * v; }
double pan(double v) { return 2.0 * v - 1.0; }
double freqHz(double v) { return 10.0 * std::pow(3000.0, v); }
double rateLog2(double v) { return -3.0 + 6.0 * v; }
double gainDb(double v) { return -30.0 + 60.0 * v; }
double q(double v) { return 0.025 * std::pow(1600.0, v); }

double freqToValue(double hz) { return dsp::clamp(std::log(hz / 10.0) / std::log(3000.0), 0.0, 1.0); }
double qToValue(double qv) { return dsp::clamp(std::log(qv / 0.025) / std::log(1600.0), 0.0, 1.0); }
double rateLog2ToValue(double r) { return dsp::clamp((r + 3.0) / 6.0, 0.0, 1.0); }
double gainDbToValue(double db) { return dsp::clamp((db + 30.0) / 60.0, 0.0, 1.0); }
} // namespace conv

double syncBeats(int mode)
{
    switch (mode)
    {
    case 1: return 1.0;
    case 2: return 0.5;
    case 3: return 0.25;
    case 4: return 0.125;
    default: return 0.0;
    }
}

namespace {

std::string fmt(const char* f, double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

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

// Parses a leading number; returns false if none. `rest` receives the
// remaining text (lower case, trimmed).
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

// Note names: "A4", "C#3", "Eb2+13", "D#5 -20".
std::optional<double> parseNote(const std::string& text)
{
    std::string t;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c)))
            t += c;
    if (t.empty())
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
    if (end == t.c_str() + i)
        return std::nullopt;
    i = static_cast<size_t>(end - t.c_str());
    double cents = 0.0;
    if (i < t.size())
    {
        char* e2 = nullptr;
        cents = std::strtod(t.c_str() + i, &e2);
        if (e2 == t.c_str() + i)
            return std::nullopt;
    }
    const int midi = static_cast<int>((octave + 1) * 12 + semi);
    return 440.0 * std::exp2((midi - 69 + cents / 100.0) / 12.0);
}

double spaceForT60(double seconds) { return dsp::spaceForT60(seconds); }

} // namespace

const ParamTable& ParamTable::get()
{
    static const ParamTable table;
    return table;
}

ParamTable::ParamTable()
{
    using namespace pid;
    const std::vector<std::string> syncLabels = {"Off", "1/4", "1/8", "1/16", "1/32"};
    auto add = [&](ParamDef d) { defs_.push_back(std::move(d)); };

    add({Space, "space", "Room", "Main", Unit::Space, 0.6});
    add({DecayRate, "decay_rate", "Length", "Main", Unit::DecayRate, 0.5});
    add({Style, "style", "Algorithm", "Main", Unit::Enum, 0.0, 3, {"Natural", "Classic", "Plate"}});
    add({Predelay, "predelay", "Pre-Delay", "Main", Unit::PredelayMs, 0.0});
    add({PredelaySync, "predelay_sync", "Pre-Delay Sync", "Main", Unit::Enum, 0.0, kNumSyncModes, syncLabels});
    add({PredelayOffset, "predelay_offset", "Pre-Delay Offset", "Main", Unit::OffsetPercent, 0.5});
    add({Character, "character", "Motion", "Main", Unit::Percent, 0.25});
    add({Brightness, "brightness", "Air", "Main", Unit::BipolarPercent, 0.5});
    add({Distance, "distance", "Depth", "Main", Unit::Percent, 0.3});
    add({Thickness, "thickness", "Density", "Main", Unit::BipolarPercent, 0.5});
    add({Ducking, "ducking", "Ducking", "Main", Unit::DuckDb, 0.0});
    add({GateEnabled, "gate_enabled", "Gate", "Main", Unit::Bool, 0.0, 2, {"Off", "On"}});
    add({GateHold, "gate_hold", "Gate Hold", "Main", Unit::GateMs, std::log(25.0) / std::log(200.0)});
    add({GateSync, "gate_sync", "Gate Sync", "Main", Unit::Enum, 0.0, kNumSyncModes, syncLabels});
    add({GateOffset, "gate_offset", "Gate Offset", "Main", Unit::OffsetPercent, 0.5});
    add({Width, "width", "Width", "Main", Unit::WidthPercent, 2.0 / 3.0});
    add({Freeze, "freeze", "Freeze", "Main", Unit::Bool, 0.0, 2, {"Off", "On"}});
    add({Mix, "mix", "Mix", "Main", Unit::Percent, 0.3});
    add({InputLevel, "input_level", "Input Level", "I/O", Unit::LevelDb, 0.5});
    add({InputPan, "input_pan", "Input Pan", "I/O", Unit::Pan, 0.5});
    add({OutputLevel, "output_level", "Output Level", "I/O", Unit::LevelDb, 0.5});
    add({OutputPan, "output_pan", "Output Pan", "I/O", Unit::Pan, 0.5});
    {
        ParamDef b{Bypass, "bypass", "Bypass", "I/O", Unit::Bool, 0.0, 2, {"Off", "On"}};
        b.isBypass = true;
        add(b);
    }

    for (int b = 0; b < dsp::kNumDecayBands; ++b)
    {
        const std::string n = std::to_string(b + 1);
        const std::string k = "decay" + n + "_";
        const std::string mod = "Decay Contour/Band " + n;
        const std::string nm = "Contour Band " + n + " ";
        ParamDef used{decay(b, DUsed), k + "used", nm + "Used", mod, Unit::Bool, 0.0, 2, {"Off", "On"}};
        used.automatable = false;
        add(used);
        add({decay(b, DEnabled), k + "enabled", nm + "Enabled", mod, Unit::Bool, 1.0, 2, {"Off", "On"}});
        add({decay(b, DShape), k + "shape", nm + "Shape", mod, Unit::Enum, 0.0, 4,
             {"Bell", "Low Shelf", "High Shelf", "Notch"}});
        add({decay(b, DFreq), k + "freq", nm + "Frequency", mod, Unit::FreqHz, conv::freqToValue(1000.0)});
        add({decay(b, DRate), k + "rate", nm + "Rate", mod, Unit::RateLog2, 0.5});
        add({decay(b, DQ), k + "q", nm + "Q", mod, Unit::Q, conv::qToValue(1.0)});
    }
    for (int b = 0; b < dsp::kNumPostBands; ++b)
    {
        const std::string n = std::to_string(b + 1);
        const std::string k = "post" + n + "_";
        const std::string mod = "Tone EQ/Band " + n;
        const std::string nm = "Tone Band " + n + " ";
        ParamDef used{post(b, PUsed), k + "used", nm + "Used", mod, Unit::Bool, 0.0, 2, {"Off", "On"}};
        used.automatable = false;
        add(used);
        add({post(b, PEnabled), k + "enabled", nm + "Enabled", mod, Unit::Bool, 1.0, 2, {"Off", "On"}});
        add({post(b, PShape), k + "shape", nm + "Shape", mod, Unit::Enum, 0.0, 5,
             {"Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut"}});
        add({post(b, PFreq), k + "freq", nm + "Frequency", mod, Unit::FreqHz, conv::freqToValue(1000.0)});
        add({post(b, PGain), k + "gain", nm + "Gain", mod, Unit::GainDb, 0.5});
        add({post(b, PQ), k + "q", nm + "Q", mod, Unit::Q, conv::qToValue(1.0)});
        add({post(b, PSlope), k + "slope", nm + "Slope", mod, Unit::Enum, 3.0, dsp::kNumSlopes,
             {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "30 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct",
              "96 dB/oct"}});
        add({post(b, PPlacement), k + "placement", nm + "Placement", mod, Unit::Enum, 0.0, 5,
             {"Stereo", "Left", "Right", "Mid", "Side"}});
    }

    uint32_t maxId = 0;
    for (const auto& d : defs_)
        maxId = std::max(maxId, d.id);
    byId_.assign(maxId + 1, -1);
    for (int i = 0; i < count(); ++i)
        byId_[defs_[static_cast<size_t>(i)].id] = i;
}

int ParamTable::indexOf(uint32_t id) const { return id < byId_.size() ? byId_[id] : -1; }

int ParamTable::indexOfKey(const std::string& key) const
{
    for (int i = 0; i < count(); ++i)
        if (defs_[static_cast<size_t>(i)].key == key)
            return i;
    return -1;
}

double ParamTable::minValue(int) const { return 0.0; }

double ParamTable::maxValue(int index) const
{
    const ParamDef& d = def(index);
    if (d.unit == Unit::Enum || d.unit == Unit::Bool)
        return d.steps - 1;
    return 1.0;
}

std::string ParamTable::toText(int index, double v) const
{
    const ParamDef& d = def(index);
    switch (d.unit)
    {
    case Unit::Percent:
    {
        const double p = v * 100.0;
        return fmt(p < 9.95 ? "%.1f%%" : "%.0f%%", p);
    }
    case Unit::BipolarPercent: return fmt("%.1f%%", (v * 2.0 - 1.0) * 100.0);
    case Unit::DuckDb: return fmt("%.2f dB", v * 24.0);
    case Unit::WidthPercent: return fmt("%.0f%%", conv::widthFrac(v) * 100.0);
    case Unit::DecayRate: return fmt("%.0f%%", conv::decayRate(v) * 100.0);
    case Unit::OffsetPercent: return fmt("%.0f%%", conv::offset(v) * 100.0);
    case Unit::PredelayMs:
    {
        const double ms = conv::predelayMs(v);
        return fmt(ms < 9.9995 ? "%.3f ms" : ms < 99.995 ? "%.2f ms" : "%.1f ms", ms);
    }
    case Unit::GateMs:
    {
        const double ms = conv::gateMs(v);
        return ms < 1000.0 ? fmt("%.0f ms", ms) : fmt("%.2f s", ms / 1000.0);
    }
    case Unit::Space:
    {
        const dsp::Room r = dsp::roomAt(v);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s, %.2f s", r.name, r.t60);
        return buf;
    }
    case Unit::LevelDb: return fmt("%+.1f dB", conv::levelDb(v));
    case Unit::Pan:
    {
        const double p = conv::pan(v) * 100.0;
        if (std::fabs(p) < 0.5)
            return "C";
        return p < 0 ? fmt("L%.0f", -p) : fmt("R%.0f", p);
    }
    case Unit::FreqHz:
    {
        const double hz = conv::freqHz(v);
        return hz < 1000.0 ? fmt(hz < 100.0 ? "%.1f Hz" : "%.0f Hz", hz) : fmt("%.2f kHz", hz / 1000.0);
    }
    case Unit::RateLog2: return fmt("%.0f%%", std::exp2(conv::rateLog2(v)) * 100.0);
    case Unit::GainDb: return fmt("%+.1f dB", conv::gainDb(v));
    case Unit::Q: return fmt("%.2f", conv::q(v));
    case Unit::Enum:
    case Unit::Bool:
    {
        const int i = static_cast<int>(std::lround(v));
        if (i >= 0 && i < static_cast<int>(d.labels.size()))
            return d.labels[static_cast<size_t>(i)];
        return std::to_string(i);
    }
    }
    return {};
}

std::optional<double> ParamTable::fromText(int index, const std::string& text) const
{
    const ParamDef& d = def(index);
    const std::string t = trim(text);
    if (t.empty())
        return std::nullopt;

    if (d.unit == Unit::Enum || d.unit == Unit::Bool)
    {
        const std::string lt = lower(t);
        for (size_t i = 0; i < d.labels.size(); ++i)
            if (lower(d.labels[i]) == lt)
                return static_cast<double>(i);
        if (d.unit == Unit::Bool)
        {
            if (lt == "on" || lt == "true" || lt == "yes")
                return 1.0;
            if (lt == "off" || lt == "false" || lt == "no")
                return 0.0;
        }
        double v;
        std::string rest;
        if (parseNumber(t, v, rest))
            return dsp::clamp(std::round(v), 0.0, static_cast<double>(d.steps - 1));
        return std::nullopt;
    }

    if (d.unit == Unit::FreqHz)
        if (auto hz = parseNote(t))
            return conv::freqToValue(*hz);
    if (d.unit == Unit::Pan)
    {
        const std::string lt = lower(t);
        if (lt == "c" || lt == "center")
            return 0.5;
        if (lt[0] == 'l' || lt[0] == 'r')
        {
            double v;
            std::string rest;
            if (!parseNumber(lt.substr(1), v, rest))
                return std::nullopt;
            const double p = dsp::clamp(v / 100.0, 0.0, 1.0) * (lt[0] == 'l' ? -1.0 : 1.0);
            return (p + 1.0) * 0.5;
        }
    }
    if (d.unit == Unit::Space)
    {
        const std::string lt = lower(t);
        const auto& rooms = dsp::roomAnchors();
        for (int i = 0; i < dsp::kNumRooms; ++i)
            if (lower(rooms[static_cast<size_t>(i)].name) == lt)
                return dsp::roomAnchorPosition(i);
        // Display format "Room Name, 2.70 s": use the decay time.
        const size_t comma = t.find(',');
        if (comma != std::string::npos)
        {
            double secs;
            std::string rest;
            if (parseNumber(t.substr(comma + 1), secs, rest))
            {
                double v = spaceForT60(dsp::clamp(secs, rooms.front().t60, rooms.back().t60));
                // Keep the value inside the named room's region so the text
                // round-trips even where the rounded time sits on a border.
                const std::string name = lower(trim(t.substr(0, comma)));
                const double half = 0.5 / (dsp::kNumRooms - 1);
                for (int i = 0; i < dsp::kNumRooms; ++i)
                    if (lower(rooms[static_cast<size_t>(i)].name) == name)
                        v = dsp::clamp(v, dsp::roomAnchorPosition(i) - half + 1e-9,
                                       dsp::roomAnchorPosition(i) + half - 1e-9);
                return dsp::clamp(v, 0.0, 1.0);
            }
        }
    }

    double v;
    std::string rest;
    if (!parseNumber(t, v, rest))
        return std::nullopt;

    const bool percent = !rest.empty() && rest[0] == '%';
    const bool percentUnit = d.unit == Unit::Percent || d.unit == Unit::BipolarPercent || d.unit == Unit::WidthPercent || d.unit == Unit::DecayRate ||
                             d.unit == Unit::OffsetPercent || d.unit == Unit::RateLog2;
    if (percent && !percentUnit)
        return dsp::clamp(v / 100.0, 0.0, 1.0); // knob position

    switch (d.unit)
    {
    case Unit::Percent: return dsp::clamp(v / 100.0, 0.0, 1.0);
    case Unit::BipolarPercent: return dsp::clamp((v / 100.0 + 1.0) * 0.5, 0.0, 1.0);
    case Unit::DuckDb: return dsp::clamp(v / 24.0, 0.0, 1.0);
    case Unit::WidthPercent: return dsp::clamp(v / 150.0, 0.0, 1.0);
    case Unit::DecayRate: return dsp::clamp((std::log2(std::max(v, 1e-3) / 100.0) + 2.0) / 4.0, 0.0, 1.0);
    case Unit::OffsetPercent: return dsp::clamp((std::log2(std::max(v, 1e-3) / 100.0) + 1.0) / 2.0, 0.0, 1.0);
    case Unit::RateLog2: return conv::rateLog2ToValue(std::log2(std::max(v, 1e-3) / 100.0));
    case Unit::PredelayMs:
    {
        const double ms = rest.rfind("s", 0) == 0 ? v * 1000.0 : v;
        return conv::predelayToValue(ms);
    }
    case Unit::GateMs:
    {
        const double ms = rest.rfind("s", 0) == 0 ? v * 1000.0 : v;
        return dsp::clamp(std::log(std::max(ms, 10.0) / 10.0) / std::log(200.0), 0.0, 1.0);
    }
    case Unit::Space:
        // A plain number is a decay time in seconds.
        return spaceForT60(dsp::clamp(v, dsp::roomAnchors().front().t60, dsp::roomAnchors().back().t60));
    case Unit::LevelDb:
    {
        const double db = rest == "x" ? 20.0 * std::log10(std::max(v, 1e-6)) : v;
        return dsp::clamp((db + 36.0) / 72.0, 0.0, 1.0);
    }
    case Unit::GainDb:
    {
        const double db = rest == "x" ? 20.0 * std::log10(std::max(v, 1e-6)) : v;
        return conv::gainDbToValue(db);
    }
    case Unit::Pan: return dsp::clamp((v / 100.0 + 1.0) * 0.5, 0.0, 1.0);
    case Unit::FreqHz:
    {
        double hz = v;
        if (!rest.empty() && rest[0] == 'k')
            hz *= 1000.0;
        return conv::freqToValue(dsp::clamp(hz, 10.0, 30000.0));
    }
    case Unit::Q: return conv::qToValue(dsp::clamp(v, 0.025, 40.0));
    default: return std::nullopt;
    }
}

dsp::EngineParams buildEngineParams(const double* values, double tempo)
{
    const ParamTable& t = ParamTable::get();
    auto val = [&](uint32_t id) { return values[t.indexOf(id)]; };
    auto ival = [&](uint32_t id) { return static_cast<int>(std::lround(val(id))); };
    using namespace pid;

    dsp::EngineParams p;
    p.space = val(Space);
    p.decayRate = conv::decayRate(val(DecayRate));
    p.style = static_cast<dsp::Style>(dsp::clamp(ival(Style), 0, 2));
    const int pdSync = ival(PredelaySync);
    if (pdSync > 0 && tempo > 0.0)
        p.predelayMs = std::min(500.0, syncBeats(pdSync) * 60000.0 / tempo * conv::offset(val(PredelayOffset)));
    else
        p.predelayMs = conv::predelayMs(val(Predelay));
    p.character = val(Character);
    p.brightness = val(Brightness);
    p.distance = val(Distance);
    p.thickness = val(Thickness);
    p.ducking = val(Ducking);
    p.gateOn = ival(GateEnabled) != 0;
    const int gSync = ival(GateSync);
    if (gSync > 0 && tempo > 0.0)
        p.gateHoldMs = syncBeats(gSync) * 60000.0 / tempo * conv::offset(val(GateOffset));
    else
        p.gateHoldMs = conv::gateMs(val(GateHold));
    p.width = conv::widthFrac(val(Width));
    p.freeze = ival(Freeze) != 0;
    p.mix = val(Mix);
    p.inGainDb = conv::levelDb(val(InputLevel));
    p.inPan = conv::pan(val(InputPan));
    p.outGainDb = conv::levelDb(val(OutputLevel));
    p.outPan = conv::pan(val(OutputPan));
    p.bypass = ival(Bypass) != 0;

    for (int b = 0; b < dsp::kNumDecayBands; ++b)
    {
        dsp::DecayBand& d = p.decayBands[static_cast<size_t>(b)];
        d.used = ival(decay(b, DUsed)) != 0;
        d.enabled = ival(decay(b, DEnabled)) != 0;
        d.shape = static_cast<dsp::DecayShape>(dsp::clamp(ival(decay(b, DShape)), 0, 3));
        d.freq = conv::freqHz(val(decay(b, DFreq)));
        d.rateLog2 = conv::rateLog2(val(decay(b, DRate)));
        d.q = conv::q(val(decay(b, DQ)));
    }
    for (int b = 0; b < dsp::kNumPostBands; ++b)
    {
        dsp::PostBand& q = p.postBands[static_cast<size_t>(b)];
        q.used = ival(post(b, PUsed)) != 0;
        q.enabled = ival(post(b, PEnabled)) != 0;
        q.shape = static_cast<dsp::PostShape>(dsp::clamp(ival(post(b, PShape)), 0, 4));
        q.freq = conv::freqHz(val(post(b, PFreq)));
        q.gainDb = conv::gainDb(val(post(b, PGain)));
        q.q = conv::q(val(post(b, PQ)));
        q.slope = dsp::clamp(ival(post(b, PSlope)), 0, dsp::kNumSlopes - 1);
        q.placement = static_cast<dsp::Placement>(dsp::clamp(ival(post(b, PPlacement)), 0, 4));
    }
    return p;
}

} // namespace aurum
