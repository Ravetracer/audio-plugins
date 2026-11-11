// docgen: the manual's generated sections, from the same sources the plugin
// is built from, so they cannot drift from it.
//
//   docgen --params-brief          the parameter table (Markdown)
//   docgen --ccmaps FILE...        the CC map library, one section per map
//
// Compiled by tools/make-manual.sh straight from the sources (release builds
// switch the offline tools off).

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "plugin/CcMap.h"
#include "plugin/Params.h"

using namespace resident;

namespace {

std::string text(uint32_t id, double v)
{
    std::string t;
    if (!paramValueText(id, v, t)) {
        char b[32];
        std::snprintf(b, sizeof(b), "%g", v);
        t = b;
    }
    return t;
}

void paramsBrief()
{
    const auto table = parameterTable();
    std::printf("| Parameter | Range | Default |\n|---|---|---|\n");
    for (size_t i = 0; i < table.size(); ++i) {
        const auto& p = table[i];
        if (p.id == pid::Level + 1 || p.id == pid::Pan + 1 || p.id > pid::CcSlot)
            continue; // folded into the first of their kind below
        if (p.id == pid::Level) {
            std::printf("| SID 1 Level ... SID 8 Level | %s to %s | %s |\n", text(p.id, p.lo).c_str(),
                        text(p.id, p.hi).c_str(), text(p.id, p.def).c_str());
        } else if (p.id == pid::Pan) {
            std::string defs;
            for (int k = 0; k < 8; ++k)
                defs += (k ? ", " : "") + text(pid::Pan + static_cast<uint32_t>(k), defaultPan(k));
            std::printf("| SID 1 Pan ... SID 8 Pan | %s to %s | %s |\n", text(p.id, p.lo).c_str(),
                        text(p.id, p.hi).c_str(), defs.c_str());
        } else if (p.id == pid::CcSlot) {
            std::printf("| CC 1 ... CC %d (named by the CC map) | 0 to 127 | 0 |\n", kCcSlots);
        } else if (p.id > pid::Level && p.id < pid::Level + 8) {
            continue;
        } else if (p.id > pid::Pan && p.id < pid::Pan + 8) {
            continue;
        } else {
            std::printf("| %s | %s to %s | %s |\n", p.name.c_str(), text(p.id, p.lo).c_str(), text(p.id, p.hi).c_str(),
                        text(p.id, p.def).c_str());
        }
    }
}

std::string labels(const CcSlot& s)
{
    std::string out;
    for (size_t k = 0; k < s.labels.size(); ++k) {
        const int from = s.labels[k].first;
        const int to = k + 1 < s.labels.size() ? s.labels[k + 1].first - 1 : 127;
        out += (k ? ", " : "") + std::to_string(from) + (to > from ? "-" + std::to_string(to) : "") + " " +
               s.labels[k].second;
    }
    return out;
}

void ccMaps(const std::vector<std::string>& files)
{
    for (const auto& f : files) {
        std::ifstream in(f);
        std::stringstream ss;
        ss << in.rdbuf();
        const CcMap m = CcMap::fromText(ss.str());
        std::printf("### %s\n\n", m.name.c_str());
        if (!m.programs.empty()) {
            std::printf("Picked when the loaded file's name contains ");
            for (size_t i = 0; i < m.programs.size(); ++i)
                std::printf("%s*%s*", i == 0 ? "" : (i + 1 == m.programs.size() ? " or " : ", "), m.programs[i].c_str());
            std::printf(". Channel %d.\n\n", m.channel + 1);
        }
        std::printf("| Slot | CC | Name | Values |\n|---:|---:|---|---|\n");
        for (int i = 0; i < kCcSlots; ++i) {
            const auto& s = m.slots[i];
            if (!s.used())
                continue;
            std::string cc = std::to_string(s.cc);
            if (s.channel >= 0)
                cc += " (ch " + std::to_string(s.channel + 1) + ")";
            std::printf("| %d | %s | %s | %s |\n", i + 1, cc.c_str(), m.slotName(i).c_str(), labels(s).c_str());
        }
        std::printf("\n");
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--params-brief") {
        paramsBrief();
        return 0;
    }
    if (mode == "--ccmaps") {
        ccMaps(std::vector<std::string>(argv + 2, argv + argc));
        return 0;
    }
    std::fprintf(stderr, "usage: docgen --params-brief | --ccmaps FILE...\n");
    return 2;
}
