#include "Keymap.h"

#include "emu/C64.h"

namespace resident::gui {

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

uint8_t letterKey(char c)
{
    static const uint8_t keys[26] = {A, B, C, D, E, F, G, H, I, J, K, L, M,
                                     N, O, P, Q, R, S, T, U, V, W, X, Y, Z};
    return keys[c - 'a'];
}

// Symbolic: the C64 key a character is on, and whether it is shifted there.
Mapping symbolic(unsigned sym)
{
    if (sym >= 'a' && sym <= 'z')
        return {static_cast<uint8_t>(letterKey(static_cast<char>(sym)))};
    if (sym >= 'A' && sym <= 'Z')
        return {static_cast<uint8_t>(letterKey(static_cast<char>(sym - 'A' + 'a'))), true};
    switch (sym) {
    case '0': return {K0};
    case '1': return {K1};
    case '2': return {K2};
    case '3': return {K3};
    case '4': return {K4};
    case '5': return {K5};
    case '6': return {K6};
    case '7': return {K7};
    case '8': return {K8};
    case '9': return {K9};
    case '!': return {K1, true};
    case '"': return {K2, true};
    case '#': return {K3, true};
    case '$': return {K4, true};
    case '%': return {K5, true};
    case '&': return {K6, true};
    case '\'': return {K7, true};
    case '(': return {K8, true};
    case ')': return {K9, true};
    case ' ': return {Space};
    case '+': return {Plus};
    case '-': return {Minus};
    case '@': return {At};
    case '*': return {Asterisk};
    case ':': return {Colon};
    case ';': return {Semicolon};
    case '=': return {Equal};
    case ',': return {Comma};
    case '.': return {Period};
    case '/': return {Slash};
    case '[': return {Colon, true};
    case ']': return {Semicolon, true};
    case '<': return {Comma, true};
    case '>': return {Period, true};
    case '?': return {Slash, true};
    case '^': return {UpArrow};          // the C64's up arrow sits where PETSCII has ^
    case 0xfe52: return {UpArrow};       // dead circumflex (German, French)
    case '_': return {LeftArrow};        // and its left arrow where it has _
    case '`': return {LeftArrow};
    case 0xfe50: return {LeftArrow};     // dead grave
    case 0xa3: return {Pound};           // the pound sign
    case '\\': return {Pound};          // PETSCII has the pound where ASCII has the backslash
    default: return {};
    }
}

} // namespace

bool Keymap::hostShiftHeld() const
{
    return held_.count(42) || held_.count(54);
}

Keymap::Press Keymap::resolve(int code, unsigned keysym) const
{
    const Mapping pos = lookup(code);
    // Keys without a character, the modifiers among them, are positional.
    const bool printable = keysym != 0 && keysym < 0xff00 && pos.pos != LShift && pos.pos != RShift &&
                           pos.pos != Ctrl && pos.pos != Commodore && pos.pos != RunStop && pos.pos != Return &&
                           pos.pos != Del;
    if (mapping == KeyMapping::Positional || !printable)
        return {pos.pos, pos.shift ? Shift::Need : Shift::Host};
    const Mapping sym = symbolic(keysym);
    if (sym.pos == None)
        return {None, Shift::Host}; // a character the C64 does not have
    if (sym.shift)
        return {sym.pos, Shift::Need};
    // The host may have needed SHIFT for it (Shift+, is ';' on a German
    // keyboard); the C64 must not see it. Letters and digits follow the host:
    // SHIFT pressed later shifts them, as on the C64.
    const bool follows = (keysym >= 'a' && keysym <= 'z') || (keysym >= '0' && keysym <= '9') || keysym == ' ';
    return {sym.pos, (!follows && hostShiftHeld()) ? Shift::Forbid : Shift::Host};
}

void Keymap::press(int code, unsigned keysym)
{
    held_[code] = resolve(code, keysym);
}

std::array<uint8_t, 8> Keymap::matrix() const
{
    std::array<uint8_t, 8> m{};
    bool need = false, forbid = false;
    std::array<uint8_t, 8> shiftKeys{};
    auto set = [](std::array<uint8_t, 8>& a, uint8_t pos) { a[pos >> 3] |= static_cast<uint8_t>(1 << (pos & 7)); };
    for (const auto& [code, k] : held_) {
        if (isJoystickKey(code, joyKeys) || k.pos == None)
            continue;
        if (k.pos == LShift || k.pos == RShift) {
            set(shiftKeys, k.pos); // the host's own SHIFT keys, below
            continue;
        }
        set(m, k.pos);
        need |= k.shift == Shift::Need;
        forbid |= k.shift == Shift::Forbid;
    }
    if (need)
        set(m, LShift);
    else if (!forbid)
        for (size_t i = 0; i < 8; ++i)
            m[i] |= shiftKeys[i];
    return m;
}

uint8_t Keymap::joystickBits() const
{
    uint8_t bits = 0;
    for (const auto& [code, press] : held_) {
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

} // namespace resident::gui
