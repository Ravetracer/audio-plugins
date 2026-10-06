#pragma once

#include <string>
#include <vector>

#include <cairo/cairo.h>

#include "Widget.h"

namespace aurum::gui {

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
    // Local file system paths, UTF-8.
    virtual void onFilesDropped(const std::vector<std::string>& paths) {}
};

} // namespace aurum::gui
