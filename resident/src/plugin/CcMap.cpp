#include "CcMap.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "EmbeddedData.h"
#include "engine/Resources.h"

namespace resident {

namespace {

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The digits at the start of `s` as a number; false when there are none or
// something other than spaces follows them (unless `rest` takes it).
bool leadingInt(const std::string& s, int& v, std::string* rest = nullptr)
{
    size_t i = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
        ++i;
    if (i == 0 || i > 6)
        return false;
    v = std::atoi(s.substr(0, i).c_str());
    const std::string r = s.substr(i);
    if (rest)
        *rest = r;
    else if (!trim(r).empty())
        return false;
    return true;
}

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        const size_t k = s.find(sep, pos);
        out.push_back(trim(s.substr(pos, k == std::string::npos ? std::string::npos : k - pos)));
        if (k == std::string::npos)
            return out;
        pos = k + 1;
    }
}

// A file name is a map name with the characters file systems dislike replaced.
std::string fileNameFor(const std::string& name)
{
    std::string f;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        f += (u < 32 || std::strchr("<>:\"/\\|?*", c)) ? '_' : c;
    }
    f = trim(f);
    while (!f.empty() && f.back() == '.')
        f.pop_back();
    return f.empty() ? "CC Map" : f;
}

} // namespace

bool CcMap::empty() const
{
    for (const auto& s : slots)
        if (s.used())
            return false;
    return true;
}

int CcMap::channelOf(int slot) const
{
    if (slot < 0 || slot >= kCcSlots)
        return channel;
    const int c = slots[slot].channel;
    return c >= 0 ? c : channel;
}

std::string CcMap::slotName(int slot) const
{
    if (slot >= 0 && slot < kCcSlots && slots[slot].used()) {
        const auto& s = slots[slot];
        return s.name.empty() ? "CC " + std::to_string(s.cc) : s.name;
    }
    return "Slot " + std::to_string(slot + 1);
}

std::string CcMap::valueText(int slot, int value) const
{
    if (slot >= 0 && slot < kCcSlots) {
        const auto& labels = slots[slot].labels;
        const std::string* found = nullptr;
        for (const auto& l : labels)
            if (value >= l.first)
                found = &l.second;
        if (found)
            return *found;
    }
    return std::to_string(value);
}

bool CcMap::valueFromText(int slot, const std::string& text, int& value) const
{
    const std::string t = lower(trim(text));
    if (slot >= 0 && slot < kCcSlots)
        for (const auto& l : slots[slot].labels)
            if (lower(l.second) == t) {
                value = l.first;
                return true;
            }
    int v;
    if (!leadingInt(t, v))
        return false;
    value = std::clamp(v, 0, 127);
    return true;
}

bool CcMap::matchesProgram(const std::string& fileName) const
{
    const std::string f = lower(fileName);
    for (const auto& p : programs)
        if (!p.empty() && f.find(p) != std::string::npos)
            return true;
    return false;
}

std::string CcMap::toText() const
{
    std::string out = "# ReSIDent CC map: slot = CC[@channel] name [| value label, value label ...]\n";
    out += "name = " + name + "\n";
    if (!programs.empty()) {
        out += "programs = ";
        for (size_t i = 0; i < programs.size(); ++i)
            out += (i ? ", " : "") + programs[i];
        out += "\n";
    }
    out += "channel = " + std::to_string(channel + 1) + "\n";
    for (int i = 0; i < kCcSlots; ++i) {
        const auto& s = slots[i];
        if (!s.used())
            continue;
        out += std::to_string(i + 1) + " = " + std::to_string(s.cc);
        if (s.channel >= 0)
            out += "@" + std::to_string(s.channel + 1);
        if (!s.name.empty())
            out += " " + s.name;
        if (!s.labels.empty()) {
            out += " |";
            for (size_t k = 0; k < s.labels.size(); ++k)
                out += (k ? ", " : " ") + std::to_string(s.labels[k].first) + " " + s.labels[k].second;
        }
        out += "\n";
    }
    return out;
}

CcMap CcMap::fromText(const std::string& text, std::string* error)
{
    CcMap m;
    int lineNo = 0;
    auto complain = [&](const std::string& what) {
        if (error && error->empty())
            *error = "line " + std::to_string(lineNo) + ": " + what;
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        ++lineNo;
        if (const size_t hash = line.find('#'); hash != std::string::npos)
            line = line.substr(0, hash);
        line = trim(line);
        if (line.empty())
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            complain("no '='");
            continue;
        }
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "name") {
            m.name = value;
        } else if (key == "programs") {
            m.programs.clear();
            for (const auto& p : split(value, ','))
                if (!p.empty())
                    m.programs.push_back(lower(p));
        } else if (key == "channel") {
            int c;
            if (leadingInt(value, c) && c >= 1 && c <= 16)
                m.channel = c - 1;
            else
                complain("channel must be 1..16");
        } else {
            int slot;
            if (!leadingInt(key, slot) || slot < 1 || slot > kCcSlots) {
                complain("unknown key '" + key + "'");
                continue;
            }
            CcSlot s;
            std::string rest;
            if (!leadingInt(value, s.cc, &rest) || s.cc > 127) {
                complain("slot " + std::to_string(slot) + ": the CC must be 0..127");
                continue;
            }
            if (!rest.empty() && rest[0] == '@') {
                int ch;
                std::string after;
                if (!leadingInt(rest.substr(1), ch, &after) || ch < 1 || ch > 16) {
                    complain("slot " + std::to_string(slot) + ": the channel must be 1..16");
                    continue;
                }
                s.channel = ch - 1;
                rest = after;
            }
            const size_t bar = rest.find('|');
            s.name = trim(rest.substr(0, bar));
            if (bar != std::string::npos) {
                for (const auto& item : split(rest.substr(bar + 1), ',')) {
                    int v;
                    std::string label;
                    if (!leadingInt(item, v, &label) || v > 127 || trim(label).empty()) {
                        complain("slot " + std::to_string(slot) + ": a label is 'value name'");
                        continue;
                    }
                    s.labels.push_back({v, trim(label)});
                }
                std::sort(s.labels.begin(), s.labels.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });
            }
            m.slots[slot - 1] = std::move(s);
        }
    }
    return m;
}

std::string ccMapFolder()
{
    if (const char* o = std::getenv("RESIDENT_CCMAP_DIR"); o && *o)
        return o;
    return joinPath(dataFolder(), "CC Maps");
}

std::vector<CcMapEntry> availableCcMaps()
{
    std::vector<CcMapEntry> out;
    for (int i = 0; embedded::kCcMaps[i]; ++i) {
        CcMapEntry e;
        e.map = CcMap::fromText(embedded::kCcMaps[i]);
        e.factory = true;
        out.push_back(std::move(e));
    }
    for (const auto& path : listFiles(ccMapFolder(), ".ccmap")) {
        std::vector<uint8_t> data;
        if (!readFile(path, data))
            continue;
        CcMapEntry e;
        e.map = CcMap::fromText(std::string(data.begin(), data.end()));
        if (e.map.name.empty())
            e.map.name = fileStem(path);
        e.path = path;
        out.push_back(std::move(e));
    }
    return out;
}

std::string saveUserCcMap(const CcMap& map, std::string& error)
{
    const std::string folder = ccMapFolder();
    if (!makeFolder(folder)) {
        error = "cannot create " + folder;
        return {};
    }
    const std::string path = joinPath(folder, fileNameFor(map.name) + ".ccmap");
    const std::string text = map.toText();
    if (!writeFile(path, std::vector<uint8_t>(text.begin(), text.end()))) {
        error = "cannot write " + path;
        return {};
    }
    return path;
}

} // namespace resident
