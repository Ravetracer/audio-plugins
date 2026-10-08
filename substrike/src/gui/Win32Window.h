#pragma once

#include <cstdint>
#include <string>

#include <cairo/cairo.h>

#include "WindowListener.h"

namespace substrike::gui {

// Win32 child window embedded into a host window, rendered with Cairo. Same
// interface as X11Window; <windows.h> stays out of this header so it does not
// leak its macros into the editor.
class Win32Window
{
public:
    explicit Win32Window(WindowListener* listener) : listener_(listener) {}
    ~Win32Window();

    bool attach(uintptr_t parent, int physW, int physH);
    void destroy();
    bool isOpen() const { return hwnd_ != nullptr; }

    void setSize(int physW, int physH);
    void show();
    void hide();
    void setScale(double s) { scale_ = s; }
    double scale() const { return scale_; }

    // Windows delivers events through the window procedure, driven by the
    // host's message loop: there is no descriptor to watch and nothing to pump.
    int fd() const { return -1; }
    void processEvents() {}
    // Redraw the whole window through an offscreen buffer.
    void render();
    void grabKeyboard();

    // Drags a file out of the window (OLE drag and drop, CF_HDROP), while the
    // left button is held. Windows runs the drag in a modal loop, so this
    // returns once it is over; the listener then gets the button release.
    bool startFileDrag(const std::string& path);
    bool dragging() const { return false; }

    // Desktop scale guess (system DPI / 96), 1.0 if unknown.
    static double systemScale();

    // Called by the window procedure. The types are LRESULT, WPARAM and
    // LPARAM spelled without <windows.h>.
    intptr_t handleMessage(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp);

private:
    void ensureBackBuffer();
    void paintBackBuffer();
    void blit(void* hdc);
    MouseEvent toMouse(intptr_t lp, uintptr_t wp) const;
    void emitChar(uint32_t codepoint);

    WindowListener* listener_;
    void* hwnd_ = nullptr;
    int width_ = 0, height_ = 0;
    double scale_ = 1.0;
    cairo_surface_t* back_ = nullptr;
    int backW_ = 0, backH_ = 0;
    unsigned buttonsDown_ = 0;
    bool trackingLeave_ = false;
    uint16_t highSurrogate_ = 0;
};

} // namespace substrike::gui
