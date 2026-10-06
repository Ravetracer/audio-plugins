#pragma once

// Key codes carried in KeyEvent::keysym. They are X11 keysym values, which the
// X11 window passes through unchanged; the Win32 window translates virtual
// keys into the same values. Printable keys use their Latin-1 code point, as
// keysyms do.
namespace aurum::gui::key {

inline constexpr unsigned BackSpace = 0xff08;
inline constexpr unsigned Tab = 0xff09;
inline constexpr unsigned Return = 0xff0d;
inline constexpr unsigned Escape = 0xff1b;
inline constexpr unsigned Home = 0xff50;
inline constexpr unsigned Left = 0xff51;
inline constexpr unsigned Up = 0xff52;
inline constexpr unsigned Right = 0xff53;
inline constexpr unsigned Down = 0xff54;
inline constexpr unsigned PageUp = 0xff55;
inline constexpr unsigned PageDown = 0xff56;
inline constexpr unsigned End = 0xff57;
inline constexpr unsigned KP_Enter = 0xff8d;
inline constexpr unsigned Delete = 0xffff;
inline constexpr unsigned BracketLeft = '[';
inline constexpr unsigned BracketRight = ']';

} // namespace aurum::gui::key
