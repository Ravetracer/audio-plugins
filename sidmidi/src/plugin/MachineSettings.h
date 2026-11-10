#pragma once

// How this instance's C64 is built: the things a user sets on the machine
// page, stored with the project. Plain text, one key = value per line, so a
// setting added later leaves older projects readable.

#include <string>

#include "emu/C64.h"
#include "emu/MidiCart.h"
#include "emu/SidBank.h"
#include "emu/Vic.h"

namespace sidmidi {

enum class JoyKeys : int { Off = 0, Numpad, Cursor };

struct MachineSettings
{
    emu::VideoStandard standard = emu::VideoStandard::PAL;
    emu::MidiInterface midi = emu::MidiInterface::Datel;
    emu::SidBankConfig sids;
    std::string palette = "colodore";
    int joyPort = 2;               // 1 or 2
    JoyKeys joyKeys = JoyKeys::Numpad;
    // What the host mouse drives over the screen, per control port.
    emu::PortDevice portDevice[2] = {emu::PortDevice::Joystick, emu::PortDevice::Joystick};
    bool sendClock = false;        // MIDI clock and start/stop from the host transport
    emu::DriveMode driveMode = emu::DriveMode::Virtual;
    bool warpOnDiskAccess = true;  // the true 1541: warp while its motor runs
    int screenScale = 2;           // 1..3, the screen's pixel size at GUI scale 1
    bool ccFollowProgram = true;   // loading a program picks its CC map

    std::string toText() const;
    void fromText(const std::string& text);
};

} // namespace sidmidi
