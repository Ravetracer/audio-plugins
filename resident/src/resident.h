#pragma once

// Identity constants. ReSIDent does not use the shared PluginCore library, but
// it follows the repository's identity and versioning conventions.
namespace resident {

// The plugin was called SidMidi up to 0.8.1. Its id stays, so hosts and
// saved projects still find it, and so does every state magic.
constexpr char kPluginId[] = "de.ravetracer.sidmidi";
constexpr char kPluginName[] = "ReSIDent";
constexpr char kPluginDirName[] = "ReSIDent";
constexpr char kPluginVendor[] = "Ravetracer";
constexpr char kPluginVersion[] = "1.1.0";
constexpr char kPluginUrl[] = "https://github.com/Ravetracer/audio-plugins";
constexpr char kPluginDescription[] = "Original C64 MIDI software, live in your DAW";

#ifdef RESIDENT_CMAKE_VERSION
constexpr bool sameString(const char* a, const char* b)
{
    return *a == *b && (*a == '\0' || sameString(a + 1, b + 1));
}
static_assert(sameString(kPluginVersion, RESIDENT_CMAKE_VERSION),
              "kPluginVersion and the CMake project() version disagree");
#endif

} // namespace resident
