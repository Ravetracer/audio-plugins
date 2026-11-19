#pragma once

#include <cstdint>
#include <string>

#include <cairo/cairo.h>

#include "WindowListener.h"

namespace substrike::gui {

// Cocoa child view embedded into a host's NSView, rendered with Cairo into an
// image surface and drawn as a CGImage. Same interface as X11Window and
// Win32Window; no Objective-C in this header, so the editor stays plain C++.
//
// Sizes are in points: CLAP's cocoa API works in logical sizes and never sets
// a scale, so systemScale() is 1 and the Retina factor is applied only when
// the view is drawn.
class CocoaWindow
{
public:
    explicit CocoaWindow(WindowListener* listener) : listener_(listener) {}
    ~CocoaWindow();

    bool attach(uintptr_t parent, int physW, int physH);
    void destroy();
    bool isOpen() const { return view_ != nullptr; }

    void setSize(int physW, int physH);
    void show();
    void hide();
    void setScale(double s) { scale_ = s; }
    double scale() const { return scale_; }

    // Cocoa delivers events through the view, driven by the host's run loop.
    int fd() const { return -1; }
    void processEvents() {}
    // Marks the view for redrawing; Cocoa draws it on the next pass.
    void render();
    void grabKeyboard();

    // Starts dragging a file out of the window (an NSDraggingSession with the
    // file's URL), while a mouse button is held. Cocoa runs the drag from then
    // on and the view sees no button release, so one is sent to the listener
    // when the drag ends, dropped or not.
    bool startFileDrag(const std::string& path);
    bool dragging() const { return dragging_; }

    static double systemScale() { return 1.0; }

    // Called by the view's methods (CocoaWindow.mm). The arguments are the
    // view's NSEvent / NSPasteboard, spelled without Objective-C; dragEnded()
    // takes the drop point in screen coordinates.
    void drawView();
    void mouseDown(void* event, int button);
    void mouseUp(void* event, int button);
    void mouseMoved(void* event);
    void mouseExited();
    void scrollWheel(void* event);
    bool keyDown(void* event);
    void filesDropped(void* pasteboard);
    void dragEnded(double screenX, double screenY);

private:
    MouseEvent toMouse(void* event) const;

    WindowListener* listener_;
    void* view_ = nullptr;       // the view (retained)
    void* colorSpace_ = nullptr; // CGColorSpaceRef, sRGB
    int width_ = 0, height_ = 0;
    double scale_ = 1.0;
    cairo_surface_t* back_ = nullptr;
    int backW_ = 0, backH_ = 0;
    // Ctrl+click is a right click on macOS; the release must match the press.
    bool ctrlClick_ = false;
    double wheelAccum_ = 0.0;
    // The mouse event being handled (NSEvent), which a drag starts from.
    void* currentEvent_ = nullptr;
    bool dragging_ = false;
};

} // namespace substrike::gui
