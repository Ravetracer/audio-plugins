// Generates the reference sections of Aurum's manual from the plugin itself.
//
// Aurum is not built on shared/, so the shared docgen (which reads a
// PluginCore ParamDesc table) cannot document it. This one reads Aurum's own
// ParamTable and its compiled-in factory presets, so the generated tables
// cannot drift from the build they describe.
//
// Built by tools/make-manual.sh, which compiles it directly with the plain
// compiler: release builds switch the test programs off, and the manual has to
// build anyway.
//
//   docgen --params-brief   every parameter, its range and default, as Markdown
//   docgen --presets        the factory preset library, as Markdown

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "plugin/Params.h"
#include "state/PresetManager.h"

using namespace aurum;

namespace {

const ParamTable& table() { return ParamTable::get(); }

std::string text(int index, double value) { return table().toText(index, value); }

// The range column: the labels for a switch or a list, the two ends otherwise.
std::string range(int index)
{
    const ParamDef& d = table().def(index);
    if ((d.unit == Unit::Enum || d.unit == Unit::Bool) && d.labels.size() > 5)
        return d.labels.front() + " to " + d.labels.back() + ", " + std::to_string(d.labels.size()) + " steps";
    if (d.unit == Unit::Enum || d.unit == Unit::Bool)
    {
        std::string out;
        for (size_t i = 0; i < d.labels.size(); ++i)
            out += (i ? ", " : "") + d.labels[i];
        return out;
    }
    if (d.unit == Unit::Space)
        return "Ambience (0.20 s) to Cathedral (10 s)";
    return text(index, table().minValue(index)) + " to " + text(index, table().maxValue(index));
}

std::string defaultText(int index) { return text(index, table().def(index).def); }

// "Decay Band 1 Frequency" -> "Frequency".
std::string bandField(const std::string& name)
{
    const size_t band = name.find("Band 1 ");
    return band == std::string::npos ? name : name.substr(band + 7);
}

void row(int index, const std::string& name)
{
    std::printf("| **%s** | %s | %s |\n", name.c_str(), range(index).c_str(), defaultText(index).c_str());
}

void groupHeader(const char* title, const char* intro = nullptr)
{
    std::printf("### %s\n\n", title);
    if (intro)
        std::printf("%s\n\n", intro);
    std::printf("| Parameter | Range | Default |\n|---|---|---|\n");
}

void group(const char* title, const std::vector<uint32_t>& ids, const char* intro = nullptr)
{
    groupHeader(title, intro);
    for (uint32_t id : ids)
    {
        const int i = table().indexOf(id);
        row(i, table().def(i).name);
    }
    std::printf("\n");
}

// One band of an EQ: band 1's fields stand for all six. The hidden "Used"
// field only records whether the band exists and is left out.
void bandGroup(const char* title, const std::string& module, const char* intro)
{
    groupHeader(title, intro);
    for (int i = 0; i < table().count(); ++i)
    {
        const ParamDef& d = table().def(i);
        if (d.module != module || !d.automatable)
            continue;
        row(i, bandField(d.name));
    }
    std::printf("\n");
}

int paramsBrief()
{
    using namespace pid;
    int all = 0, bandParams = 0;
    for (int i = 0; i < table().count(); ++i)
    {
        const ParamDef& d = table().def(i);
        if (!d.automatable)
            continue;
        ++all;
        if (d.module.find("/Band") != std::string::npos)
            ++bandParams;
    }
    std::printf("Aurum has %d automatable parameters: %d main controls and %d for the\n"
                "twelve EQ bands. They are listed here the way the window groups them.\n\n",
                all, all - bandParams, bandParams);
    group("Room", {Space, DecayRate, Style, Predelay, PredelaySync, PredelayOffset});
    group("Character", {Character, Brightness, Distance, Thickness});
    group("Output", {Width, Ducking, GateEnabled, GateHold, GateSync, GateOffset, Freeze, Mix});
    group("Input / Output", {InputLevel, InputPan, OutputLevel, OutputPan, Bypass});
    bandGroup("Decay Contour bands", "Decay Contour/Band 1",
              "Six bands, each with these. **Rate** multiplies the reverb time around the band's frequency.");
    bandGroup("Tone EQ bands", "Tone EQ/Band 1",
              "Six bands, each with these. **Slope** is for the cuts, **Gain** for the bells and shelves.");
    return 0;
}

// "02 Halls" -> "Halls": the number only orders the folders.
std::string folderName(const std::string& f)
{
    if (f.size() > 3 && std::isdigit(static_cast<unsigned char>(f[0])) &&
        std::isdigit(static_cast<unsigned char>(f[1])) && f[2] == ' ')
        return f.substr(3);
    return f;
}

std::string tagList(const std::string& tags)
{
    std::string out;
    for (const std::string& t : PresetManager::splitTags(tags))
        out += (out.empty() ? "" : ", ") + t;
    return out;
}

int presets()
{
    const auto list = factoryPresets();
    std::printf("%zu factory presets come with Aurum, in the folders the browser shows them in.\n\n",
                list.size());
    std::string lastFolder = "\x01";
    for (const auto& [rel, doc] : list)
    {
        const size_t slash = rel.rfind('/');
        const std::string folder = slash == std::string::npos ? std::string() : rel.substr(0, slash);
        const std::string name = slash == std::string::npos ? rel : rel.substr(slash + 1);
        // Presets outside a folder (Init) come first, under no heading.
        if (folder != lastFolder && !folder.empty())
            std::printf("#### %s\n\n", folderName(folder).c_str());
        lastFolder = folder;
        const auto desc = doc.meta.find("description");
        const auto tags = doc.meta.find("tags");
        std::printf("**%s**\n: %s", name.c_str(), desc == doc.meta.end() ? "" : desc->second.c_str());
        if (tags != doc.meta.end() && !tags->second.empty())
            std::printf(" *(%s)*", tagList(tags->second).c_str());
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
