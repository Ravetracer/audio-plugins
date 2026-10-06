#include "StateIO.h"

#include <cstdio>
#include <cstdlib>
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

} // namespace

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
    out << "[Aurum]\nformat=1\n";
    for (const auto& [k, v] : doc.meta)
        out << k << '=' << v << '\n';
    out << "[Parameters]\n";
    char buf[64];
    for (int i = 0; i < t.count() && i < static_cast<int>(doc.values.size()); ++i)
    {
        std::snprintf(buf, sizeof(buf), "%.9g", doc.values[static_cast<size_t>(i)]);
        out << t.def(i).key << '=' << buf << '\n';
    }
    return out.str();
}

bool parseState(const std::string& text, StateDocument& doc)
{
    const ParamTable& t = ParamTable::get();
    doc.values = defaultValues();
    doc.meta.clear();
    std::istringstream in(text);
    std::string line;
    std::string section;
    bool seenHeader = false;
    while (std::getline(in, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = line.substr(1, line.size() - 2);
            seenHeader |= section == "Aurum";
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (section == "Aurum")
        {
            if (key != "format")
                doc.meta[key] = value;
        }
        else if (section == "Parameters")
        {
            const int idx = t.indexOfKey(key);
            if (idx < 0)
                continue;
            char* end = nullptr;
            const double v = std::strtod(value.c_str(), &end);
            if (end != value.c_str())
                doc.values[static_cast<size_t>(idx)] = std::min(std::max(v, t.minValue(idx)), t.maxValue(idx));
        }
    }
    return seenHeader;
}

} // namespace aurum
