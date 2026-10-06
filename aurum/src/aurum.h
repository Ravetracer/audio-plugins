#pragma once

// Identity constants. Aurum does not use the shared PluginCore library, but it
// follows the repository's identity and versioning conventions.
namespace aurum {

constexpr char kPluginId[] = "de.ravetracer.aurum";
// What the host shows.
constexpr char kPluginName[] = "Aurum Reverb";
// The name for anything on disk: the CMake project, the bundles and the
// settings/preset folders (XDG_CONFIG_HOME and XDG_DATA_HOME on Linux,
// %APPDATA% on Windows).
constexpr char kPluginDirName[] = "Aurum";
constexpr char kPluginVendor[] = "Ravetracer";
constexpr char kPluginVersion[] = "0.3.0";
constexpr char kPluginUrl[] = "https://github.com/Ravetracer/audio-plugins";
constexpr char kPluginDescription[] = "Algorithmic reverb with a frequency-dependent decay contour";

// The build fails if the version here and the one in CMakeLists.txt disagree.
#ifdef AURUM_CMAKE_VERSION
constexpr bool sameString(const char* a, const char* b)
{
    return *a == *b && (*a == '\0' || sameString(a + 1, b + 1));
}
static_assert(sameString(kPluginVersion, AURUM_CMAKE_VERSION),
              "kPluginVersion and the CMake project() version disagree");
#endif

} // namespace aurum
