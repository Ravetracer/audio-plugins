#pragma once

// A complete picture of the machine: CPU, RAM and colour RAM, the processor
// port, VIC, both CIAs, the MIDI cartridge with what is on its line, the
// cartridge port with its image, the virtual drive with its disk, and every
// SID's reSIDfp state. Loading one puts the C64 back exactly where it was,
// with the program running.
//
// The format is this build's: the structures are stored as they are in
// memory, each with its size, so a snapshot from a build where any of them
// changed is refused instead of misread (the caller then cold-boots the
// attached media instead). reSIDfp's own state is likewise not portable
// across builds; when it does not fit, the SIDs restart with their registers
// rewritten.

#include <cstdint>
#include <vector>

namespace sidmidi::emu {

class C64;

std::vector<uint8_t> saveSnapshot(const C64& c64);
bool loadSnapshot(C64& c64, const std::vector<uint8_t>& data);

} // namespace sidmidi::emu
