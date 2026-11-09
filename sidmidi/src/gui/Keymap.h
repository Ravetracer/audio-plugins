#pragma once

// From the host keyboard to the C64's: positional, by scan code, so the keys
// sit where a C64 has them whatever the host's layout (on a German keyboard
// as on a US one). Some C64 keys need SHIFT held (cursor left/up, F2/F4/F6/F8,
// INST); those press the left SHIFT along.
//
// The state is rebuilt from the set of held host keys on every change, so a
// key released in any order can never leave a C64 key stuck.
//
//   Esc        RUN/STOP          Tab        CTRL
//   Left Ctrl  C=                ` (grave)  <-
//   - =        + -               [ ]        @ *
//   ; '        : ;               \ (or #)   =
//   Home       CLR/HOME          Insert     INST
//   Delete     DEL               End, <>    pound
//   Page Down  arrow up          Page Up    RESTORE
//   F1..F8     F1..F8 (even = shifted)

#include <array>
#include <cstdint>
#include <set>

#include "plugin/MachineSettings.h"

namespace sidmidi::gui {

class Keymap
{
public:
    JoyKeys joyKeys = JoyKeys::Numpad;

    // Returns true when the key is one of RESTORE's (handled by the caller).
    static bool isRestore(int code) { return code == 104; }

    void press(int code) { held_.insert(code); }
    void release(int code) { held_.erase(code); }
    void clear() { held_.clear(); }

    // The C64 matrix and joystick bits the held keys make.
    std::array<uint8_t, 8> matrix() const;
    uint8_t joystickBits() const;

private:
    std::set<int> held_;
};

} // namespace sidmidi::gui
