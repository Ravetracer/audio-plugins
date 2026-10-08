#include "StateIO.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "plugin/Params.h"

namespace substrike {

namespace {

std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace

std::string curveKey(int index)
{
    if (index >= dsp::kNumLaneCurves)
        return "mod.env" + std::to_string(index - dsp::kNumLaneCurves + 1);
    return "l" + std::to_string(index / dsp::kCurvesPerLane + 1) +
           (index % dsp::kCurvesPerLane == dsp::PitchCurve ? ".pitch" : ".amp");
}

std::string curveToText(const dsp::Curve& c)
{
    std::string out;
    char buf[96];
    for (int i = 0; i < c.count(); ++i)
    {
        const dsp::CurvePoint& p = c.point(i);
        std::snprintf(buf, sizeof(buf), "%s%.6g,%.6g,%.6g", i ? ";" : "", p.x, p.y, p.k);
        out += buf;
    }
    return out;
}

bool curveFromText(const std::string& text, dsp::Curve& c)
{
    std::vector<dsp::CurvePoint> points;
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find(';', start);
        if (end == std::string::npos)
            end = text.size();
        const std::string item = trim(text.substr(start, end - start));
        start = end + 1;
        if (item.empty())
            continue;
        dsp::CurvePoint p;
        double* fields[3] = {&p.x, &p.y, &p.k};
        const char* cur = item.c_str();
        for (int f = 0; f < 3; ++f)
        {
            char* stop = nullptr;
            *fields[f] = std::strtod(cur, &stop);
            if (stop == cur)
                return false;
            cur = stop;
            while (*cur == ' ')
                ++cur;
            if (f < 2)
            {
                if (*cur != ',')
                    return false;
                ++cur;
            }
        }
        if (static_cast<int>(points.size()) == dsp::Curve::kMaxPoints)
            break;
        points.push_back(p);
    }
    if (points.size() < 2)
        return false;
    c.set(points.data(), static_cast<int>(points.size()));
    return true;
}

std::vector<double> defaultValues()
{
    const ParamTable& t = ParamTable::get();
    std::vector<double> v(static_cast<size_t>(t.count()));
    for (int i = 0; i < t.count(); ++i)
        v[static_cast<size_t>(i)] = t.def(i).def;
    return v;
}

std::string serializeState(const StateDocument& doc)
{
    const ParamTable& t = ParamTable::get();
    std::ostringstream out;
    out << "[Substrike]\nformat=1\n";
    for (const auto& [k, v] : doc.meta)
        out << k << '=' << v << '\n';
    out << "[Parameters]\n";
    char buf[64];
    for (int i = 0; i < t.count() && i < static_cast<int>(doc.values.size()); ++i)
    {
        // A slot letter is written under the key and in the unit its slot's
        // type gives it, so "l1.slot1.drive=12" rather than a knob position.
        const ParamDef& d = t.effective(i, doc.values.data());
        const double v = doc.values[static_cast<size_t>(i)];
        if (d.kind == Kind::Continuous && d.choices.empty())
        {
            double plain = ParamTable::toPlain(d, v);
            // Rounding in the knob mapping leaves 0 as -1.3e-15; write 0.
            if (std::fabs(plain) < 1e-9 * (d.hi - d.lo))
                plain = 0.0;
            std::snprintf(buf, sizeof(buf), "%.9g", plain);
            out << d.key << '=' << buf << '\n';
        }
        else
            out << d.key << '=' << ParamTable::toText(d, v) << '\n';
    }
    out << "[Curves]\n";
    for (int i = 0; i < dsp::kNumCurves; ++i)
        out << curveKey(i) << '=' << curveToText(doc.curves[static_cast<size_t>(i)]) << '\n';
    return out.str();
}

bool parseState(const std::string& text, StateDocument& doc)
{
    const ParamTable& t = ParamTable::get();
    doc.values = defaultValues();
    doc.meta.clear();
    doc.curves.fill(dsp::Curve());
    doc.warnings.clear();
    std::istringstream in(text);
    std::string line;
    std::string section;
    bool seenHeader = false;
    std::vector<std::pair<std::string, std::string>> params;
    while (std::getline(in, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = line.substr(1, line.size() - 2);
            seenHeader |= section == "Substrike";
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (section == "Substrike")
        {
            if (key != "format")
                doc.meta[key] = value;
        }
        else if (section == "Parameters")
            params.emplace_back(key, value);
        else if (section == "Curves")
        {
            bool known = false;
            for (int i = 0; i < dsp::kNumCurves; ++i)
                if (key == curveKey(i))
                {
                    known = true;
                    dsp::Curve c;
                    if (curveFromText(value, c))
                        doc.curves[static_cast<size_t>(i)] = c;
                    else
                        doc.warnings.push_back("unreadable curve " + key);
                }
            if (!known)
                doc.warnings.push_back("unknown curve " + key);
        }
    }

    auto apply = [&](int idx, const ParamDef& d, const std::string& value) {
        if (d.kind == Kind::Continuous && d.choices.empty())
        {
            char* end = nullptr;
            const double v = std::strtod(value.c_str(), &end);
            if (end != value.c_str())
            {
                doc.values[static_cast<size_t>(idx)] = ParamTable::fromPlain(d, v);
                // -inf is how a silent level is written.
                if (std::isfinite(v) && (v < d.lo - 1e-9 * std::fabs(d.lo) || v > d.hi + 1e-9 * std::fabs(d.hi)))
                    doc.warnings.push_back(d.key + "=" + value + " is outside " + std::to_string(d.lo) + ".." +
                                           std::to_string(d.hi));
            }
            else
                doc.warnings.push_back(d.key + "=" + value + " is not a number");
        }
        else if (auto v = ParamTable::fromText(d, value))
            doc.values[static_cast<size_t>(idx)] = *v;
        else
            doc.warnings.push_back(d.key + "=" + value + " is not one of its choices");
    };

    // Everything but the slot letters first, the slot types among it; then
    // every letter starts at its type's default, and the letters in the text
    // are read with the meaning their type gives them.
    for (const auto& [key, value] : params)
    {
        const int idx = t.indexOfKey(key);
        if (idx >= 0 && t.def(idx).typeParam < 0)
            apply(idx, t.def(idx), value);
    }
    for (int i = 0; i < t.count(); ++i)
        if (t.def(i).typeParam >= 0)
            doc.values[static_cast<size_t>(i)] = t.effective(i, doc.values.data()).def;
    for (const auto& [key, value] : params)
    {
        const int idx = t.indexOfSlotKey(key, doc.values.data());
        if (idx >= 0)
            apply(idx, t.effective(idx, doc.values.data()), value);
        else if (t.indexOfKey(key) < 0)
            doc.warnings.push_back("unknown key " + key);
    }
    return seenHeader;
}

} // namespace substrike
