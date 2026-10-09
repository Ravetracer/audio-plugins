#pragma once

// Identity constants. Substrike does not use the shared PluginCore library, but
// it follows the repository's identity and versioning conventions.
namespace substrike {

constexpr char kPluginId[] = "de.ravetracer.substrike";
// What the host shows.
constexpr char kPluginName[] = "Substrike";
// The name for anything on disk: the CMake project, the bundles and the
// settings/preset folders.
constexpr char kPluginDirName[] = "Substrike";
constexpr char kPluginVendor[] = "Ravetracer";
constexpr char kPluginVersion[] = "1.1.0";
constexpr char kPluginUrl[] = "https://github.com/Ravetracer/audio-plugins";
constexpr char kPluginDescription[] = "Layered kick drum designer";

// The build fails if the version here and the one in CMakeLists.txt disagree.
#ifdef SUBSTRIKE_CMAKE_VERSION
constexpr bool sameString(const char* a, const char* b)
{
    return *a == *b && (*a == '\0' || sameString(a + 1, b + 1));
}
static_assert(sameString(kPluginVersion, SUBSTRIKE_CMAKE_VERSION),
              "kPluginVersion and the CMake project() version disagree");
#endif

} // namespace substrike
