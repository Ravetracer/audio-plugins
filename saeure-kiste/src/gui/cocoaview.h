#pragma once

// The window's macOS backend: an NSView child embedded into the host's view.
//
// The window (seqwindow.cpp) stays plain C++ and draws into a Cairo image
// surface exactly as it does on the other platforms; this view only puts that
// surface on screen and hands the events back. No Objective-C in this header,
// so seqwindow.cpp compiles as C++ on every platform.
//
// Sizes are in points. CLAP's cocoa API works in logical sizes and never sets
// a scale; the Retina factor is the view's business, handed to the window as
// the `backing` of each frame so the surface is drawn at device resolution.

#include <cstdint>
#include <string>

#include <cairo/cairo.h>

namespace saeurekiste {

// What the window does with the view's events. The calls arrive on the main
// thread, from the host's run loop.
class CocoaViewListener {
public:
   virtual ~CocoaViewListener() = default;

   // Draws the frame for a view whose backing store has `backing` device
   // pixels per point, and returns the image surface it is in. The surface
   // belongs to the window and stays valid until the next call.
   virtual cairo_surface_t *cocoaRender(double backing) = 0;

   // Coordinates in points from the view's top left. Buttons follow X11's
   // numbering, as the window does everywhere: 1 left, 3 right, 4/5 wheel.
   virtual void cocoaMouseDown(double x, double y, unsigned button, unsigned long timeMs,
                               bool shift) = 0;
   virtual void cocoaMouseUp() = 0;
   virtual void cocoaMouseMoved(double x, double y, bool shift) = 0;
   virtual void cocoaMouseExited() = 0;

   // Only while the window holds the keyboard (takeKeyboard). Returns whether
   // the key was used; an unused one goes on to the host.
   enum class Key { None, Escape, Accept, Backspace, Up, Down };
   virtual bool cocoaKey(Key key, const char *text, int length) = 0;

   // The view lost the keyboard without being asked to give it back.
   virtual void cocoaFocusLost() = 0;
};

class CocoaView {
public:
   explicit CocoaView(CocoaViewListener &listener) : mListener(listener) {}
   ~CocoaView();

   CocoaView(const CocoaView &) = delete;
   CocoaView &operator=(const CocoaView &) = delete;

   // Makes the view, not yet in any window. embed() puts it into the host's.
   bool create(int width, int height);
   bool embed(uintptr_t parentView);
   void destroy();
   bool isOpen() const { return mView != nullptr; }
   void *nsView() const { return mView; }

   void setSize(int width, int height);
   void show();
   void hide();
   // Marks the view for redrawing; Cocoa calls cocoaRender on its next pass.
   void invalidate();

   // Makes the view the window's first responder, so typed keys reach it.
   // releaseKeyboard hands the focus back to whatever had it before.
   bool takeKeyboard();
   void releaseKeyboard();

   // Called by the view class in cocoaview.mm. The arguments are NSEvent* and
   // NSWindow*, spelled without Objective-C.
   void drawView();
   void mouseDown(void *event, unsigned button);
   void mouseUp(void *event, unsigned button);
   void mouseMoved(void *event);
   void mouseExited();
   void scrollWheel(void *event);
   bool keyDown(void *event);
   void focusLost();
   void observeWindow(void *window);
   bool wantsKeys() const { return mWantsKeys; }

private:
   CocoaViewListener &mListener;
   void *mView = nullptr;           // the NSView, retained
   void *mColorSpace = nullptr;     // CGColorSpaceRef, sRGB
   void *mPrevResponder = nullptr;  // NSResponder*, not retained
   void *mResignObserver = nullptr; // NSWindowDidResignKeyNotification token
   bool mWantsKeys = false;
   // Control+click is a right click on macOS; the release must match it.
   bool mCtrlClick = false;
   double mWheelAccum = 0.0;
};

} // namespace saeurekiste
