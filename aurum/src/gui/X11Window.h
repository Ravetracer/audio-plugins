#pragma once

#include <cstdint>
#include <string>

#include <cairo/cairo.h>

#include "WindowListener.h"

typedef struct _XDisplay Display;

namespace aurum::gui {

// X11 child window embedded into a host window, rendered with Cairo.
class X11Window
{
public:
    explicit X11Window(WindowListener* listener) : listener_(listener) {}
    ~X11Window();

    bool attach(uintptr_t parent, int physW, int physH);
    void destroy();
    bool isOpen() const { return window_ != 0; }

    void setSize(int physW, int physH);
    void show();
    void hide();
    void setScale(double s) { scale_ = s; }
    double scale() const { return scale_; }

    int fd() const;
    void processEvents();
    // Redraw the whole window through an offscreen buffer.
    void render();
    void grabKeyboard();

    // Desktop scale guess (Xft.dpi / 96), 1.0 if unknown.
    static double systemScale();

private:
    void handleEvent(void* xevent);
    void ensureBackBuffer();
    MouseEvent toMouse(int x, int y, unsigned state) const;
    void handleXdndEnter(void* xevent);
    void handleXdndPosition(void* xevent);
    void handleXdndDrop(void* xevent);
    void handleSelectionNotify(void* xevent);

    WindowListener* listener_;
    Display* display_ = nullptr;
    unsigned long window_ = 0;
    unsigned long parent_ = 0;
    int width_ = 0, height_ = 0;
    double scale_ = 1.0;
    cairo_surface_t* xsurface_ = nullptr;
    cairo_surface_t* back_ = nullptr;
    int backW_ = 0, backH_ = 0;
    // XDND
    unsigned long xdndSource_ = 0;
    int xdndVersion_ = 0;
    unsigned long atoms_[16] = {};
};

} // namespace aurum::gui
