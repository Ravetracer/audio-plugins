#pragma once

#include <string>
#include <vector>

#include <cairo/cairo.h>

#include "Widget.h"

namespace resident::gui {

// Receives window-system events (logical coordinates are computed by the
// window from its scale factor).
class WindowListener
{
public:
    virtual ~WindowListener() = default;
    virtual void onPaint(cairo_t* cr, int physW, int physH) = 0;
    virtual void onMouseDown(const MouseEvent& e) = 0;
    virtual void onMouseUp(const MouseEvent& e) = 0;
    virtual void onMouseMove(const MouseEvent& e) = 0;
    virtual void onMouseWheel(const MouseEvent& e) = 0;
    virtual void onMouseLeave() = 0;
    virtual bool onKey(const KeyEvent& e) = 0;
    // Every key press and release, as a Linux evdev key code (the same as a PC
    // set-1 scan code for the main block; the Win32 window translates the
    // extended keys). Repeats arrive as repeated presses. A press also carries
    // the keysym the host's layout gives that key with the modifiers held at
    // that moment (X11 keysym values, 0 when there is none); a release does not.
    virtual void onRawKey(int code, bool down, unsigned keysym) {}
    // The window lost the keyboard: anything held must be let go.
    virtual void onFocusLost() {}
    // Local file system paths, UTF-8.
    virtual void onFilesDropped(const std::vector<std::string>& paths) {}
};

} // namespace resident::gui
