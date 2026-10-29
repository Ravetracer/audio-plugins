#pragma once

#include <string>

#include "dsp/curve.h"
#include "plugincore/preset.h"

namespace shapeau {

// The preset format, PresetData and the directory conventions come from the
// PluginCore shared library next door; this plugin supplies its own name,
// extension, parameter table, and the shape curves that the shared format
// does not know about (see preset.cpp).
using namespace plugincore;

constexpr char kPluginId[] = "de.ravetracer.shapeau";
constexpr char kPluginName[] = "Shapeau";

// The name everything on disk uses: the binary, the install folder and the
// user preset directory. Kept separate from kPluginName because a label is not
// a path, even when the two happen to agree today.
constexpr char kPluginDirName[] = "Shapeau";
constexpr char kPluginVendor[] = "Ravetracer";
constexpr char kPluginVersion[] = "0.3.0";

// Nothing else keeps the two version sites in step, and drift here is quiet.
// The build fails if the version here and the one in CMakeLists.txt disagree.
#ifdef SHAPEAU_CMAKE_VERSION
constexpr bool sameString(const char *a, const char *b) {
   return *a == *b && (*a == '\0' || sameString(a + 1, b + 1));
}
static_assert(sameString(kPluginVersion, SHAPEAU_CMAKE_VERSION),
              "kPluginVersion and the CMake project() version disagree");
#endif

constexpr char kPluginUrl[] = "https://github.com/Ravetracer/audio-plugins";
constexpr char kPluginDescription[] =
   "A multiband drawing effects rack: seven stages in series -- volume, filter, drive, "
   "crush, time, width and pan -- each with its own drawable curve for every one of "
   "three frequency bands. Draw a ducking shape, a filter sweep, a distortion envelope, "
   "a stepped crush pattern, a stutter or a stereo move into the curve editor and it "
   "runs locked to the host's beat.";

constexpr char kPresetExtension[] = "shapeau";
constexpr char kProviderId[] = "de.ravetracer.shapeau.preset-provider";

// The plugin's own preset context, and the preset calls bound to it. The four
// overloads that take a CurveSet are the ones that carry the shapes: the
// shared format ignores keys it does not recognise, so they ride through it as
// extra lines the same way SäureKiste's patterns do.
const PresetContext &presetContext();

std::string factoryPresetDir();
std::string userPresetDir();
bool parsePreset(const char *text, size_t length, PresetData &out, std::string &error);
bool parsePresetFile(const std::string &path, PresetData &out, std::string &error);
std::string formatPreset(const PresetData &preset);
std::string userPresetPath(const std::string &name);

// The same three, carrying the twelve shape curves as well. `curves` is reset
// to neutral before the file is read, so a preset without shape lines loads
// with every module flat.
bool parsePreset(const char *text, size_t length, PresetData &out, CurveSet &curves,
                 std::string &error);
bool parsePresetFileWithCurves(const std::string &path, PresetData &out, CurveSet &curves,
                               std::string &error);
std::string formatPreset(const PresetData &preset, const CurveSet &curves);

using plugincore::writePresetFile;
using plugincore::userPresetPathIn;

} // namespace shapeau
