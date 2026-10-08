#pragma once

#include <cstdint>
#include <string>

#include <cairo/cairo.h>

#include "WindowListener.h"

typedef struct _XDisplay Display;

namespace substrike::gui {

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

    // Starts dragging a file out of the window, while a mouse button is held
    // (XDND source). It runs from the event loop: the drop happens when the
    // button is released over a window that accepts it. The button release
    // still reaches the listener.
    bool startFileDrag(const std::string& path);
    bool dragging() const { return drag_.active; }

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
    // Drag source
    unsigned long findDropTarget(int rootX, int rootY, int& version);
    void dragMotion(int rootX, int rootY, unsigned long time);
    void dragRelease(unsigned long time);
    void dragStatus(void* xevent);
    void dragSendPosition();
    void dragSend(unsigned long target, int message, long l1, long l2, long l3, long l4);
    void dragEnd();
    void answerSelectionRequest(void* xevent);

    WindowListener* listener_;
    Display* display_ = nullptr;
    unsigned long window_ = 0;
    unsigned long parent_ = 0;
    int width_ = 0, height_ = 0;
    double scale_ = 1.0;
    cairo_surface_t* xsurface_ = nullptr;
    cairo_surface_t* back_ = nullptr;
    int backW_ = 0, backH_ = 0;
    // XDND, as a target
    unsigned long xdndSource_ = 0;
    int xdndVersion_ = 0;
    unsigned long atoms_[16] = {};
    // XDND, as a source
    struct Drag
    {
        bool active = false;
        std::string uri;
        unsigned long target = 0;
        int version = 5;
        bool accepted = false;
        bool waitingStatus = false;
        bool positionQueued = false;
        bool dropPending = false; // released while a status was outstanding
        bool dropped = false;
        double droppedAt = 0.0;
        int x = 0, y = 0;
        unsigned long time = 0;
    } drag_;
    unsigned long lastTime_ = 0;
};

} // namespace substrike::gui
