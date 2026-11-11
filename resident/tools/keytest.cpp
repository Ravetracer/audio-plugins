// resident-keytest: the keyboard mapping, symbolic and positional, with the
// key codes and keysyms a German layout produces.

#include <cstdio>
#include <string>

#include "emu/C64.h"
#include "gui/Keymap.h"

using namespace resident;
using namespace resident::emu::keys;

namespace {

int failures = 0;
void check(bool ok, const std::string& what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++failures;
}

bool down(const std::array<uint8_t, 8>& m, uint8_t pos) { return (m[pos >> 3] >> (pos & 7)) & 1; }
int count(const std::array<uint8_t, 8>& m)
{
    int n = 0;
    for (uint8_t b : m)
        for (int i = 0; i < 8; ++i)
            n += (b >> i) & 1;
    return n;
}

// evdev codes
constexpr int kShiftL = 42, kComma = 51, kAe = 40, k7 = 8, k8 = 9, kA = 30, kLeft = 105, kAltGr = 100;

} // namespace

int main()
{
    gui::Keymap km;
    km.mapping = KeyMapping::Symbolic;

    // Shift+, gives ';' on a German keyboard: the C64's ';', without SHIFT.
    km.press(kShiftL, 0xffe1);
    km.press(kComma, ';');
    auto m = km.matrix();
    check(down(m, Semicolon) && !down(m, LShift) && !down(m, RShift) && count(m) == 1,
          "symbolic: Shift+, (';') is the C64's ';' without SHIFT");
    km.release(kShiftL);
    m = km.matrix();
    check(down(m, Semicolon) && count(m) == 1, "releasing SHIFT first keeps the ';'");
    km.release(kComma);
    check(count(km.matrix()) == 0, "and releasing the key leaves nothing held");

    // A character the C64 does not have.
    km.press(kAe, 0xe4);
    check(count(km.matrix()) == 0, "symbolic: 'ä' does nothing");
    km.release(kAe);

    // Shift+7 is '/' on a German keyboard (no SHIFT on the C64), Shift+8 is '('.
    km.press(kShiftL, 0xffe1);
    km.press(k7, '/');
    m = km.matrix();
    check(down(m, Slash) && !down(m, LShift) && count(m) == 1, "symbolic: Shift+7 ('/') is the C64's '/'");
    km.release(k7);
    km.press(k8, '(');
    m = km.matrix();
    check(down(m, K8) && down(m, LShift) && count(m) == 2, "symbolic: Shift+8 ('(') is SHIFT+8 on the C64");
    km.release(k8);
    km.release(kShiftL);

    // AltGr+8 is '[' on a German keyboard: SHIFT+: on the C64.
    km.press(kAltGr, 0xfe03);
    km.press(k8, '[');
    m = km.matrix();
    check(down(m, Colon) && down(m, LShift) && count(m) == 2, "symbolic: AltGr+8 ('[') is SHIFT+: on the C64");
    km.release(k8);
    km.release(kAltGr);

    // Letters follow SHIFT, as on the C64.
    km.press(kA, 'a');
    m = km.matrix();
    check(down(m, A) && count(m) == 1, "symbolic: 'a' is A");
    km.press(kShiftL, 0xffe1);
    m = km.matrix();
    check(down(m, A) && down(m, LShift), "SHIFT pressed after a letter shifts it");
    km.release(kA);
    km.release(kShiftL);

    // Keys without a character are positional in both.
    km.press(kLeft, 0xff51);
    m = km.matrix();
    check(down(m, CrsrRight) && down(m, LShift) && count(m) == 2, "symbolic: cursor left is SHIFT+CRSR right");
    km.release(kLeft);

    // Positional: the key in its place, whatever it prints.
    km.mapping = KeyMapping::Positional;
    km.press(kAe, 0xe4);
    m = km.matrix();
    check(down(m, Semicolon) && count(m) == 1, "positional: the key two right of L (ä on a German keyboard) is the C64's ';'");
    km.release(kAe);
    km.press(kShiftL, 0xffe1);
    km.press(kComma, ';');
    m = km.matrix();
    check(down(m, Comma) && down(m, LShift) && count(m) == 2, "positional: Shift+, is SHIFT+, on the C64");
    km.release(kComma);
    km.release(kShiftL);

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
