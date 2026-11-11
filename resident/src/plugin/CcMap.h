#pragma once

// CC maps: which MIDI controllers a C64 program listens to, as named
// parameter slots for the host.
//
// The plugin has kCcSlots automatable parameters with fixed ids and a fixed
// 0..127 range. A map gives each slot a name, the controller it sends and,
// optionally, names for value ranges ("Off"/"On", waveforms); loading another
// map renames the slots (CLAP_PARAM_RESCAN_INFO, which the VST3 wrapper turns
// into kParamTitlesChanged), so automation stays bound to the slot.
//
// The text format, one line per slot (comments with #):
//
//     name = SID Synth
//     programs = sidsynth, sid synth    # matched in the loaded file's name
//     channel = 1                        # the default MIDI channel, 1..16
//     1 = 74 Cutoff
//     7 = 81 Vib On | 0 Off, 64 On       # a value names the range from it up
//     9 = 28@2 Waveform | 0 Tri, 32 Saw  # CC 28 on channel 2
//
// Factory maps are embedded from ccmaps/*.ccmap; the user's are *.ccmap
// files in ccMapFolder().

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace resident {

// Station64 listens to 53 controllers, the most of the supported programs.
// 64 is also the width of the plugin's dirty mask (one uint64_t).
inline constexpr int kCcSlots = 64;

struct CcSlot
{
    int cc = -1;       // 0..127, -1 = unused
    int channel = -1;  // 0..15, -1 = the map's
    std::string name;
    std::vector<std::pair<int, std::string>> labels; // from value, ascending

    bool used() const { return cc >= 0; }
};

struct CcMap
{
    std::string name;
    std::vector<std::string> programs; // lower case
    int channel = 0;                   // 0..15
    CcSlot slots[kCcSlots];

    bool empty() const;
    // The slot's MIDI channel (0..15).
    int channelOf(int slot) const;
    // "Cutoff", or "Slot 12" for an unused slot.
    std::string slotName(int slot) const;
    // The value's label, or the number.
    std::string valueText(int slot, int value) const;
    // A typed text back to a value: a label, or a number.
    bool valueFromText(int slot, const std::string& text, int& value) const;
    // Whether the loaded file's name is one of this map's programs.
    bool matchesProgram(const std::string& fileName) const;

    std::string toText() const;
    // Lines it cannot read are reported in `error` and skipped.
    static CcMap fromText(const std::string& text, std::string* error = nullptr);
};

// Where the user's maps live: $XDG_DATA_HOME/ReSIDent/CC Maps on Linux,
// %APPDATA%\ReSIDent\CC Maps on Windows, ~/Library/Application Support/ReSIDent/
// CC Maps on macOS. RESIDENT_CCMAP_DIR overrides it.
std::string ccMapFolder();

struct CcMapEntry
{
    CcMap map;
    bool factory = false;
    std::string path; // the user's file
};

// The factory maps, then the user's (read from the folder on every call).
// Following a program, a user's map that names it wins over a factory one.
std::vector<CcMapEntry> availableCcMaps();
// Writes a map to the user's folder as <name>.ccmap. Returns the path, or
// empty with `error` set.
std::string saveUserCcMap(const CcMap& map, std::string& error);

} // namespace resident
