#pragma once

// From the host keyboard to the C64's, in one of two ways:
//
// Symbolic (the default): a key types the character the host's layout gives
// it -- on a German keyboard Shift+, is ';', and the C64 gets its ';' key,
// without SHIFT. SHIFT is pressed or held back for the C64 as the character
// needs. Characters the C64 does not have do nothing.
//
// Positional: by scan code, so the keys sit where a C64 has them whatever
// the host's layout (on a German keyboard as on a US one).
//
// Keys without a character (cursor keys, F-keys, RETURN, DEL, RUN/STOP, the
// modifiers) are positional in both. Some C64 keys need SHIFT held (cursor
// left/up, F2/F4/F6/F8, INST); those press the left SHIFT along.
//
// A key's meaning is fixed when it goes down, and the state is rebuilt from
// the held keys on every change, so a key released in any order can never
// leave a C64 key stuck.
//
// Positional layout:
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
#include <map>

#include "plugin/MachineSettings.h"

namespace resident::gui {

class Keymap
{
public:
    JoyKeys joyKeys = JoyKeys::Numpad;
    KeyMapping mapping = KeyMapping::Symbolic;

    // Returns true when the key is one of RESTORE's (handled by the caller).
    static bool isRestore(int code) { return code == 104; }

    // `keysym`: what the host's layout gives the key now (0 for none).
    void press(int code, unsigned keysym);
    void release(int code) { held_.erase(code); }
    void clear() { held_.clear(); }

    // What a held key does on the C64: a key, and SHIFT as the host's
    // (Host), pressed (Need) or held back (Forbid).
    enum class Shift : uint8_t { Host, Need, Forbid };
    struct Press
    {
        uint8_t pos;
        Shift shift;
    };
    // For tests: what a key would do pressed now.
    Press resolve(int code, unsigned keysym) const;

    // The C64 matrix and joystick bits the held keys make.
    std::array<uint8_t, 8> matrix() const;
    uint8_t joystickBits() const;

private:
    bool hostShiftHeld() const;
    std::map<int, Press> held_;
};

} // namespace resident::gui
