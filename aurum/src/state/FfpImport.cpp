#include "FfpImport.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>

#include "plugin/Params.h"

namespace aurum {

namespace {

std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string unquote(const std::string& s)
{
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

// .ffp files store Q normalised as 400^(x - 0.5) (0.5 = Q 1); convert to ours.
double qValue(double x) { return conv::qToValue(std::pow(400.0, x - 0.5)); }

} // namespace

bool importFfpPreset(const std::string& text, StateDocument& out, std::string* error)
{
    std::map<std::string, std::string> meta, params;
    std::istringstream in(text);
    std::string line, section;
    while (std::getline(in, line))
    {
        line = trim(line);
        if (line.empty())
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        (section == "Parameters" ? params : meta)[k] = v;
    }
    if (meta["Signature"] != "FR2p")
    {
        if (error)
            *error = "Not a supported .ffp preset";
        return false;
    }

    out.values = defaultValues();
    out.meta.clear();
    const ParamTable& t = ParamTable::get();
    auto has = [&](const std::string& k) { return params.count(k) != 0; };
    auto num = [&](const std::string& k, double def = 0.0) {
        auto it = params.find(k);
        return it == params.end() ? def : std::strtod(it->second.c_str(), nullptr);
    };
    auto set = [&](uint32_t id, double v) { out.values[static_cast<size_t>(t.indexOf(id))] = v; };
    using namespace pid;

    if (has("Space"))
        set(Space, clamp01(num("Space")));
    if (has("Decay Rate")) // -1..1, multiplier 4^x (25 % .. 400 %)
        set(DecayRate, clamp01((num("Decay Rate") + 1.0) * 0.5));
    if (has("Style"))
        set(Style, std::clamp(std::round(num("Style")), 0.0, 2.0));
    if (has("Predelay")) // knob position 0..1
        set(Predelay, clamp01(num("Predelay")));
    if (has("Predelay Sync"))
        set(PredelaySync, std::clamp(std::round(num("Predelay Sync")), 0.0, 4.0));
    if (has("Predelay Offset")) // log2
        set(PredelayOffset, clamp01((num("Predelay Offset") + 1.0) / 2.0));
    if (has("Character"))
        set(Character, clamp01(num("Character")));
    if (has("Brightness"))
        set(Brightness, clamp01(num("Brightness")));
    if (has("Distance"))
        set(Distance, clamp01(num("Distance")));
    if (has("Thickness")) // -1..1
        set(Thickness, clamp01((num("Thickness") + 1.0) * 0.5));
    if (has("Ducking")) // dB range
        set(Ducking, clamp01(num("Ducking") / 24.0));
    if (has("Stereo Width"))
        set(Width, clamp01(num("Stereo Width") / 1.5));
    if (has("Mix")) // percent
        set(Mix, clamp01(num("Mix") / 100.0));
    if (has("Auto Gate Enabled"))
        set(GateEnabled, num("Auto Gate Enabled") > 0.5 ? 1.0 : 0.0);
    if (has("Auto Gate")) // hold, ms
        set(GateHold, clamp01(std::log(std::max(num("Auto Gate"), 10.0) / 10.0) / std::log(200.0)));
    if (has("Auto Gate Sync"))
        set(GateSync, std::clamp(std::round(num("Auto Gate Sync")), 0.0, 4.0));
    if (has("Auto Gate Offset"))
        set(GateOffset, clamp01((num("Auto Gate Offset") + 1.0) / 2.0));
    if (has("Input Level"))
        set(InputLevel, clamp01((num("Input Level") + 36.0) / 72.0));
    if (has("Output Level"))
        set(OutputLevel, clamp01((num("Output Level") + 36.0) / 72.0));
    if (has("Input Pan"))
        set(InputPan, clamp01((num("Input Pan") + 1.0) * 0.5));
    if (has("Output Pan"))
        set(OutputPan, clamp01((num("Output Pan") + 1.0) * 0.5));

    for (int b = 0; b < 6; ++b)
    {
        const std::string d = "Decay EQ Band " + std::to_string(b + 1) + " ";
        if (has(d + "Used"))
        {
            set(decay(b, DUsed), num(d + "Used") > 0.5 ? 1.0 : 0.0);
            set(decay(b, DEnabled), num(d + "Enabled", 1) > 0.5 ? 1.0 : 0.0);
            set(decay(b, DShape), std::clamp(std::round(num(d + "Shape")), 0.0, 3.0));
            set(decay(b, DFreq), conv::freqToValue(std::exp2(num(d + "Frequency", 9.965784))));
            set(decay(b, DRate), conv::rateLog2ToValue(num(d + "Rate")));
            set(decay(b, DQ), qValue(num(d + "Q", 0.5)));
        }
        const std::string p = "Post EQ Band " + std::to_string(b + 1) + " ";
        if (has(p + "Used"))
        {
            // Shape order: Bell, Low Shelf, Low Cut, High Shelf, High Cut.
            static const int shapeMap[5] = {0, 1, 3, 2, 4};
            static const int placeMap[5] = {1, 2, 0, 3, 4}; // L, R, Stereo, M, S
            const int shape = std::clamp(static_cast<int>(std::lround(num(p + "Shape"))), 0, 4);
            const int place = std::clamp(static_cast<int>(std::lround(num(p + "Stereo Placement", 2))), 0, 4);
            set(post(b, PUsed), num(p + "Used") > 0.5 ? 1.0 : 0.0);
            set(post(b, PEnabled), num(p + "Enabled", 1) > 0.5 ? 1.0 : 0.0);
            set(post(b, PShape), shapeMap[shape]);
            set(post(b, PFreq), conv::freqToValue(std::exp2(num(p + "Frequency", 9.965784))));
            set(post(b, PGain), conv::gainDbToValue(num(p + "Gain")));
            set(post(b, PQ), qValue(num(p + "Q", 0.5)));
            set(post(b, PSlope), std::clamp(std::round(num(p + "Slope", 1)), 0.0, 8.0));
            set(post(b, PPlacement), placeMap[place]);
        }
    }

    if (!unquote(meta["Author"]).empty())
        out.meta["author"] = unquote(meta["Author"]);
    if (!unquote(meta["Tags"]).empty())
        out.meta["tags"] = unquote(meta["Tags"]);
    if (!unquote(meta["Description"]).empty())
        out.meta["description"] = unquote(meta["Description"]);
    out.meta["imported_from"] = "ffp";
    return true;
}

} // namespace aurum
