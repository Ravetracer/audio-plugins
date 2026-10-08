// Generates the reference sections of Substrike's manual from the plugin
// itself: its parameter table and its compiled-in factory presets, so the
// tables cannot drift from the build they describe.
//
// Built by tools/make-manual.sh with the plain compiler: release builds
// switch the tools off, and the manual has to build anyway.
//
//   docgen --params-brief   the parameters, their ranges and defaults, as Markdown
//   docgen --presets        the factory preset library, as Markdown

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "plugin/Params.h"
#include "state/Presets.h"

using namespace substrike;

namespace {

const ParamTable& table() { return ParamTable::get(); }

// The range column: the choices for a short list, the ends of a long one or
// of a continuous range.
std::string range(const ParamDef& d)
{
    const std::vector<std::string>& names = d.kind == Kind::Continuous ? d.choices : d.labels;
    if (!names.empty())
    {
        if (names.size() > 8)
            return names.front() + " to " + names.back() + ", " + std::to_string(names.size()) + " steps";
        std::string out;
        for (size_t i = 0; i < names.size(); ++i)
            out += (i ? ", " : "") + names[i];
        return out;
    }
    const double lo = d.kind == Kind::Continuous ? 0.0 : d.lo, hi = d.kind == Kind::Continuous ? 1.0 : d.hi;
    return ParamTable::toText(d, lo) + " to " + ParamTable::toText(d, hi);
}

// "L1 Pitch Start" -> "Pitch Start", "L1 Slot 1 Drive" -> "Drive",
// "Master Slot 1 Mix" -> "Mix", "LFO 1 Rate" -> "Rate", "Mod 1 Amount" -> "Amount".
std::string shortName(const std::string& name)
{
    const size_t slot = name.find("Slot ");
    if (slot != std::string::npos)
    {
        const size_t sp = name.find(' ', slot + 5);
        if (sp != std::string::npos)
            return name.substr(sp + 1);
    }
    for (const char* prefix : {"L1 ", "LFO 1 ", "Env 1 ", "Mod 1 "})
        if (name.rfind(prefix, 0) == 0)
            return name.substr(std::strlen(prefix));
    return name;
}

void header(const char* title, const char* intro = nullptr)
{
    std::printf("### %s\n\n", title);
    if (intro)
        std::printf("%s\n\n", intro);
    std::printf("| Parameter | Range | Default |\n|---|---|---|\n");
}

void row(const ParamDef& d, const std::string& name)
{
    std::printf("| **%s** | %s | %s |\n", name.c_str(), range(d).c_str(), ParamTable::toText(d, d.def).c_str());
}

// Every parameter of one module, in table order.
void module(const char* title, const std::string& mod, const char* intro = nullptr)
{
    header(title, intro);
    bool busDone = false;
    for (int i = 0; i < table().count(); ++i)
    {
        const ParamDef& d = table().def(i);
        if (d.module != mod)
            continue;
        // The eight bus switches as one row.
        if (d.name.find("Bus Lane ") != std::string::npos)
        {
            if (!busDone)
                std::printf("| **Bus Lane 1-8** | Off, On | Lane 1 on |\n");
            busDone = true;
            continue;
        }
        row(d, shortName(d.name));
    }
    std::printf("\n");
}

int paramsBrief()
{
    int automatable = 0;
    for (int i = 0; i < table().count(); ++i)
        automatable += table().def(i).automatable;
    std::printf("Substrike has %d parameters, %d of them automatable. Most of them repeat: eight lanes\n"
                "with the same controls, six slots on each lane and on the master, four LFOs, four\n"
                "envelopes and 32 routes. Each is listed once here; in the host they carry their lane,\n"
                "slot or number in front (\"L3 Slot 2 Cutoff\", \"LFO 2 Rate\").\n\n",
                table().count(), automatable);

    module("Master", "Master");
    module("Lane", "Lane 1", "Every lane has these, the transient guard among them.");
    module("Body", "Lane 1/Body");
    module("Click", "Lane 1/Click");
    module("Noise", "Lane 1/Noise");
    module("Resonator", "Lane 1/Resonator");
    module("Bus", "Lane 1/Bus");

    // A slot: the four fixed controls, then the letters as each type names them.
    header("Effect slot", "Every slot, on a lane or on the master. Type is a setting, not automatable.");
    const uint32_t base = pid::slot(0, 0, 0);
    for (uint32_t f : {static_cast<uint32_t>(pid::SType), static_cast<uint32_t>(pid::SBand),
                       static_cast<uint32_t>(pid::SMix), static_cast<uint32_t>(pid::SBypass)})
    {
        const ParamDef& d = table().def(table().indexOf(base + f));
        row(d, shortName(d.name));
    }
    std::printf("\n");
    const ParamDef& type = table().def(table().indexOf(base + pid::SType));
    std::printf("### Effect controls by type\n\n"
                "A slot's six generic controls take their name, range and default from its type.\n\n");
    for (size_t t = 1; t < type.labels.size(); ++t)
    {
        std::printf("**%s**\n\n| Control | Range | Default |\n|---|---|---|\n", type.labels[t].c_str());
        for (int k = 0; k < dsp::kSlotValues; ++k)
        {
            const int i = table().indexOf(base + pid::SValueA + static_cast<uint32_t>(k));
            const ParamDef& d = table().effective(i, static_cast<double>(t));
            if (d.key == table().def(i).key)
                continue; // a letter this type does not use
            row(d, shortName(d.name));
        }
        std::printf("\n");
    }

    module("Macros", "Modulation/Macros");
    module("LFO", "Modulation/LFO 1", "Four of them, each with these.");
    module("Envelope", "Modulation/Env 1", "Four of them, each with these and its drawn curve.");
    header("Matrix route", "32 of them, each with these.");
    for (uint32_t f : {static_cast<uint32_t>(pid::RSource), static_cast<uint32_t>(pid::RDest),
                       static_cast<uint32_t>(pid::RAmount), static_cast<uint32_t>(pid::RCurve)})
    {
        const ParamDef& d = table().def(table().indexOf(pid::route(0, static_cast<pid::RouteField>(f))));
        if (d.isDestination)
            std::printf("| **Destination** | any continuous parameter above | None |\n");
        else
            row(d, shortName(d.name));
    }
    std::printf("\n");
    return 0;
}

int presets()
{
    const auto& list = factoryPresets();
    std::printf("%zu factory presets come with Substrike, in the categories the browser shows.\n\n", list.size());
    std::string last;
    for (const PresetInfo& p : list)
    {
        if (p.category != last)
            std::printf("#### %s\n\n", p.category.c_str());
        last = p.category;
        std::printf("**%s**\n: %s", p.name.c_str(), p.description.c_str());
        std::string tags;
        for (const std::string& t : p.tags)
            tags += (tags.empty() ? "" : ", ") + t;
        if (!tags.empty())
            std::printf(" *(%s)*", tags.c_str());
        std::printf("\n\n");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && !std::strcmp(argv[1], "--params-brief"))
        return paramsBrief();
    if (argc == 2 && !std::strcmp(argv[1], "--presets"))
        return presets();
    std::fprintf(stderr, "usage: docgen --params-brief | --presets\n");
    return 1;
}
