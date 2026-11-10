#pragma once

// SidMidi's automatable parameters: ids, names, ranges and defaults, and
// their text. The plugin and the manual's parameter table (tools/docgen.cpp)
// read the same table.

#include <cstdint>
#include <string>
#include <vector>

#include "plugin/CcMap.h"

namespace sidmidi {

namespace pid {
// CcSlot + i is CC slot i (0..kCcSlots-1): ids never change, the map only
// renames them.
enum : uint32_t { Master = 1, Level = 100, Pan = 200, CcSlot = 1000 };
constexpr int kCount = 1 + 2 * 8 + kCcSlots;
inline bool isCcSlot(uint32_t id) { return id >= CcSlot && id < CcSlot + kCcSlots; }
} // namespace pid

struct ParamInfo
{
    uint32_t id;
    const char* module;
    std::string name;
    double lo, hi, def;
    bool stepped = false;
};

// Every parameter, in host order. CC slots are named after an empty map.
std::vector<ParamInfo> parameterTable();
// The default pan of SID i: centre for SID 1, then alternating right and left.
double defaultPan(int sid);

// Text for the volume and level (dB) and pan parameters; CC slots take their
// text from the map. Returns false for an id it does not know.
bool paramValueText(uint32_t id, double v, std::string& out);

} // namespace sidmidi
