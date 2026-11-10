#pragma once

// Identity constants. SidMidi does not use the shared PluginCore library, but
// it follows the repository's identity and versioning conventions.
namespace sidmidi {

constexpr char kPluginId[] = "de.ravetracer.sidmidi";
constexpr char kPluginName[] = "SidMidi";
constexpr char kPluginDirName[] = "SidMidi";
constexpr char kPluginVendor[] = "Ravetracer";
constexpr char kPluginVersion[] = "0.5.0";
constexpr char kPluginUrl[] = "https://github.com/Ravetracer/audio-plugins";
constexpr char kPluginDescription[] = "A C64 running its MIDI programs, played from the DAW";

#ifdef SIDMIDI_CMAKE_VERSION
constexpr bool sameString(const char* a, const char* b)
{
    return *a == *b && (*a == '\0' || sameString(a + 1, b + 1));
}
static_assert(sameString(kPluginVersion, SIDMIDI_CMAKE_VERSION),
              "kPluginVersion and the CMake project() version disagree");
#endif

} // namespace sidmidi
