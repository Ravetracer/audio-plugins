#pragma once

#include <string>

#include "plugincore/preset.h"

#include "pattern.h"

namespace saeurekiste {

// The preset format, PresetData and the directory conventions come from the
// PluginCore shared library next door; this plugin only supplies its own name,
// extension and parameter table.
using namespace plugincore;

constexpr char kPluginId[] = "de.ravetracer.saeurekiste";
// What the host shows, and the only place the umlaut belongs: it is a label,
// not a path. Everything a compiler, a shell or a filesystem has to parse uses
// kPluginDirName below.
constexpr char kPluginName[] = "SäureKiste";

// The same name without the umlaut, for anything that ends up on disk. The
// preset code looks for the factory presets beside the binary as
// "<dir>/<name>.clap/presets", so this has to be exactly the CMake project name
// -- SaeureKiste -- or a released build finds no presets at all. It is also
// what the user preset directory is called, which keeps ~/.config in step with
// ~/.clap instead of having one of each spelling.
constexpr char kPluginDirName[] = "SaeureKiste";
constexpr char kPluginVendor[] = "Ravetracer";
constexpr char kPluginVersion[] = "0.15.0";

// Nothing else keeps the two version sites in step, and drift here is quiet.
// The build fails if the version here and the one in CMakeLists.txt disagree.
#ifdef SAEUREKISTE_CMAKE_VERSION
constexpr bool sameString(const char *a, const char *b) {
   return *a == *b && (*a == '\0' || sameString(a + 1, b + 1));
}
static_assert(sameString(kPluginVersion, SAEUREKISTE_CMAKE_VERSION),
              "kPluginVersion and the CMake project() version disagree");
#endif

constexpr char kPluginUrl[] = "https://github.com/Ravetracer/audio-plugins";
constexpr char kPluginDescription[] =
   "A monophonic bass synthesiser modelled on the 1982 Roland TB-303 service notes: "
   "a falling-ramp VCO, the four-stage transistor ladder with its unequal capacitors, "
   "the envelope-modulation bias trick, the accent sweep and its 68 ms time constant, "
   "and an overdrive stage after it. Played from the host: velocity makes an accent, "
   "overlapping notes make a slide.";

constexpr char kPresetExtension[] = "saeurekiste";
constexpr char kProviderId[] = "de.ravetracer.saeurekiste.preset-provider";

// The plugin's own preset context, and the preset calls bound to it. Thin
// wrappers over the shared implementations so that call sites read unchanged.
//
// With one addition. This plugin has a sixteen-step pattern, and the shared
// preset format only knows about the parameter table -- it silently ignores any
// key that is not a parameter, which is what keeps older presets loadable. That
// is exactly the hook needed here: the pattern is written as five extra lines
// which the shared writer never produces and the shared reader never reads, and
// the overloads below add and pick them up on the way past. Nothing in
// shared/ changes, which matters because every plugin in the tree uses it.
const PresetContext &presetContext();

std::string factoryPresetDir();
std::string userPresetDir();
bool parsePreset(const char *text, size_t length, PresetData &out, std::string &error);
bool parsePresetFile(const std::string &path, PresetData &out, std::string &error);
std::string formatPreset(const PresetData &preset);
std::string userPresetPath(const std::string &name);

// The same three, carrying the pattern as well. A null PatternData* on the way
// in means "do not look"; `present` on the way out says whether the preset had
// one at all, and a preset without one leaves the pattern alone.
bool parsePreset(const char *text, size_t length, PresetData &out, PatternData *pattern,
                 std::string &error);
bool parsePresetFile(const std::string &path, PresetData &out, PatternData *pattern,
                     std::string &error);
std::string formatPreset(const PresetData &preset, const PatternData *pattern);
using plugincore::writePresetFile;

} // namespace saeurekiste
