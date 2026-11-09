#include "Keymap.h"

#include "emu/C64.h"

namespace sidmidi::gui {

namespace {

using namespace emu::keys;

struct Mapping
{
    uint8_t pos = None;
    bool shift = false;
};

// Linux evdev codes (= PC set-1 scan codes for the main block).
Mapping lookup(int code)
{
    switch (code) {
    case 1: return {RunStop};
    case 2: return {K1};
    case 3: return {K2};
    case 4: return {K3};
    case 5: return {K4};
    case 6: return {K5};
    case 7: return {K6};
    case 8: return {K7};
    case 9: return {K8};
    case 10: return {K9};
    case 11: return {K0};
    case 12: return {Plus};
    case 13: return {Minus};
    case 14: return {Del};
    case 15: return {Ctrl};
    case 16: return {Q};
    case 17: return {W};
    case 18: return {E};
    case 19: return {R};
    case 20: return {T};
    case 21: return {Y};
    case 22: return {U};
    case 23: return {I};
    case 24: return {O};
    case 25: return {P};
    case 26: return {At};
    case 27: return {Asterisk};
    case 28: return {Return};
    case 29: return {Commodore};
    case 30: return {A};
    case 31: return {S};
    case 32: return {D};
    case 33: return {F};
    case 34: return {G};
    case 35: return {H};
    case 36: return {J};
    case 37: return {K};
    case 38: return {L};
    case 39: return {Colon};
    case 40: return {Semicolon};
    case 41: return {LeftArrow};
    case 42: return {LShift};
    case 43: return {Equal};
    case 44: return {Z};
    case 45: return {X};
    case 46: return {C};
    case 47: return {V};
    case 48: return {B};
    case 49: return {N};
    case 50: return {M};
    case 51: return {Comma};
    case 52: return {Period};
    case 53: return {Slash};
    case 54: return {RShift};
    case 57: return {Space};
    case 58: return {LShift}; // caps lock as SHIFT LOCK, held
    case 59: return {F1};
    case 60: return {F1, true};
    case 61: return {F3};
    case 62: return {F3, true};
    case 63: return {F5};
    case 64: return {F5, true};
    case 65: return {F7};
    case 66: return {F7, true};
    case 86: return {Pound};
    case 102: return {Home};
    case 103: return {CrsrDown, true};
    case 105: return {CrsrRight, true};
    case 106: return {CrsrRight};
    case 107: return {Pound};
    case 108: return {CrsrDown};
    case 109: return {UpArrow};
    case 110: return {Del, true};
    case 111: return {Del};
    default: return {};
    }
}

bool isJoystickKey(int code, JoyKeys mode)
{
    if (mode == JoyKeys::Numpad)
        return (code >= 71 && code <= 82 && code != 74 && code != 78) || code == 96;
    if (mode == JoyKeys::Cursor)
        return code == 103 || code == 105 || code == 106 || code == 108 || code == 97 || code == 100;
    return false;
}

} // namespace

std::array<uint8_t, 8> Keymap::matrix() const
{
    std::array<uint8_t, 8> m{};
    bool shift = false;
    for (int code : held_) {
        if (isJoystickKey(code, joyKeys))
            continue;
        const Mapping k = lookup(code);
        if (k.pos == None)
            continue;
        m[k.pos >> 3] |= static_cast<uint8_t>(1 << (k.pos & 7));
        shift |= k.shift;
    }
    if (shift)
        m[LShift >> 3] |= static_cast<uint8_t>(1 << (LShift & 7));
    return m;
}

uint8_t Keymap::joystickBits() const
{
    uint8_t bits = 0;
    for (int code : held_) {
        if (!isJoystickKey(code, joyKeys))
            continue;
        if (joyKeys == JoyKeys::Numpad) {
            switch (code) {
            case 72: bits |= emu::JoyUp; break;
            case 80: bits |= emu::JoyDown; break;
            case 75: bits |= emu::JoyLeft; break;
            case 77: bits |= emu::JoyRight; break;
            case 71: bits |= emu::JoyUp | emu::JoyLeft; break;
            case 73: bits |= emu::JoyUp | emu::JoyRight; break;
            case 79: bits |= emu::JoyDown | emu::JoyLeft; break;
            case 81: bits |= emu::JoyDown | emu::JoyRight; break;
            case 82: case 76: case 96: bits |= emu::JoyFire; break;
            default: break;
            }
        } else {
            switch (code) {
            case 103: bits |= emu::JoyUp; break;
            case 108: bits |= emu::JoyDown; break;
            case 105: bits |= emu::JoyLeft; break;
            case 106: bits |= emu::JoyRight; break;
            case 97: case 100: bits |= emu::JoyFire; break;
            default: break;
            }
        }
    }
    return bits;
}

} // namespace sidmidi::gui
