#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <cairo/cairo.h>

#include "WindowListener.h"

namespace resident::gui {

// The window of the WebAssembly editor (the WCLAP's ui.wasm): an image surface
// the page copies into a canvas. The page feeds the input events in through
// the exported functions in web/UiMain.cpp and picks up each rendered frame as
// RGBA bytes. Same interface as X11Window and Win32Window.
class WebWindow
{
public:
    explicit WebWindow(WindowListener* listener) : listener_(listener) {}
    ~WebWindow();

    bool attach(uintptr_t parent, int physW, int physH);
    void destroy();
    bool isOpen() const { return open_; }

    void setSize(int physW, int physH);
    void show() {}
    void hide() {}
    void setScale(double s) { scale_ = s; }
    double scale() const { return scale_; }

    int fd() const { return -1; }
    void processEvents() {}
    void render();
    void grabKeyboard() {}

    // The page's devicePixelRatio, set before the editor is created.
    static double systemScale();
    static void setSystemScale(double s);

    // Input from the page, in physical (canvas) pixels.
    void pointerDown(float x, float y, int button, unsigned mods, int clicks);
    void pointerUp(float x, float y, int button, unsigned mods);
    void pointerMove(float x, float y, unsigned mods);
    void wheel(float x, float y, float notches, unsigned mods);
    void leave();
    bool key(unsigned keysym, const char* text, unsigned mods);
    // Every press and release with an evdev code, as the desktop windows
    // report them (the page translates keysyms, web/UiMain.cpp).
    void rawKey(int code, bool down, unsigned keysym) { listener_->onRawKey(code, down, keysym); }
    void filesDropped(const std::vector<std::string>& paths) { listener_->onFilesDropped(paths); }

    // The last rendered frame: RGBA, width * height * 4 bytes.
    const uint8_t* frame() const { return rgba_.data(); }
    int frameWidth() const { return frameW_; }
    int frameHeight() const { return frameH_; }
    uint32_t frameSerial() const { return serial_; }

private:
    MouseEvent toMouse(float x, float y, unsigned mods) const;

    WindowListener* listener_;
    bool open_ = false;
    int width_ = 0, height_ = 0;
    double scale_ = 1.0;
    cairo_surface_t* back_ = nullptr;
    int backW_ = 0, backH_ = 0;
    std::vector<uint8_t> rgba_;
    int frameW_ = 0, frameH_ = 0;
    uint32_t serial_ = 0;
    int buttons_ = 0;
};

} // namespace resident::gui
