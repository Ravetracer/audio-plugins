#pragma once

#include <string>

#include "plugincore/preset.h"

#include "pattern.h"

namespace rumpelkiste {

// The preset format, PresetData and the directory conventions come from the
// PluginCore shared library next door; this plugin only supplies its own name,
// extension and parameter table.
using namespace plugincore;

constexpr char kPluginId[] = "de.ravetracer.rumpelkiste";
// What the host shows. The same as the directory name this time -- there is no
// umlaut to lose -- but the two are kept apart anyway, because SäureKiste
// showed that they do not have to agree and that everything on disk has to use
// the second.
constexpr char kPluginName[] = "RumpelKiste";

// The name for anything that ends up on disk. The preset code looks for the
// factory presets beside the binary as "<dir>/<name>.clap/presets", so this has
// to be exactly the CMake project name, and it is what the user preset
// directory is called.
constexpr char kPluginDirName[] = "RumpelKiste";
constexpr char kPluginVendor[] = "Ravetracer";
constexpr char kPluginVersion[] = "0.2.1";

// Nothing else keeps the two version sites in step, and drift here is quiet.
// The build fails if the version here and the one in CMakeLists.txt disagree.
#ifdef RUMPELKISTE_CMAKE_VERSION
constexpr bool sameString(const char *a, const char *b) {
   return *a == *b && (*a == '\0' || sameString(a + 1, b + 1));
}
static_assert(sameString(kPluginVersion, RUMPELKISTE_CMAKE_VERSION),
              "kPluginVersion and the CMake project() version disagree");
#endif

constexpr char kPluginUrl[] = "https://github.com/Ravetracer/audio-plugins";
constexpr char kPluginDescription[] =
   "A rhythm composer modelled on the 1984 Roland TR-909 service notes: the bass drum's "
   "reset triangle core and its two pitch envelopes, the snare's two VCOs and split noise "
   "path, three-oscillator toms, the rim shot's three bridged-T resonators, the clap's "
   "four bursts, and six-bit cymbals and hi-hats. Played from the host over MIDI or from "
   "its own step sequencer with sixty-four patterns, accents, flams and shuffle.";

constexpr char kPresetExtension[] = "rumpelkiste";
constexpr char kProviderId[] = "de.ravetracer.rumpelkiste.preset-provider";

// The plugin's own preset context, and the preset calls bound to it. Thin
// wrappers over the shared implementations so that call sites read unchanged.
//
// The pattern bank is carried the way SäureKiste carries its own: the shared
// preset format ignores any key that is not a parameter, so the bank is written
// as extra lines the shared writer never produces and the shared reader never
// reads, and the overloads below add and pick them up on the way past. Nothing
// in shared/ changes.
const PresetContext &presetContext();

std::string factoryPresetDir();
std::string userPresetDir();
bool parsePreset(const char *text, size_t length, PresetData &out, std::string &error);
bool parsePresetFile(const std::string &path, PresetData &out, std::string &error);
std::string formatPreset(const PresetData &preset);
std::string userPresetPath(const std::string &name);

// The same three, carrying the bank as well. A null PatternData* on the way in
// means "do not look"; `present` on the way out says whether the preset had one
// at all, and a preset without one leaves the bank alone.
bool parsePreset(const char *text, size_t length, PresetData &out, PatternData *pattern,
                 std::string &error);
bool parsePresetFile(const std::string &path, PresetData &out, PatternData *pattern,
                     std::string &error);
std::string formatPreset(const PresetData &preset, const PatternData *pattern);
using plugincore::writePresetFile;

} // namespace rumpelkiste
