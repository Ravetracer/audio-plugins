#pragma once

// Shapeau's window. The parameter model, the preset browser and the Gui /
// GuiDelegate interfaces are the shared ones; the window itself is a fork of
// shared/src/gui/window.cpp (see the top of window.cpp for why), and this
// header is where the plugin and that fork meet.

#include "plugincore/gui/gui.h"
#include "plugincore/gui/window.h"

#include "gui/curveaccess.h"

namespace shapeau {

using plugincore::Gui;
using plugincore::GuiDelegate;
using plugincore::GuiPreset;

// Creates the window, or returns nullptr when no display could be opened.
Gui *createGui(GuiDelegate &delegate, CurveAccess &curves);

#if defined(__wasm__)
// The window of the WCLAP's editor (web/uimain.cpp): the same window, drawn
// into an image the page copies into its canvas, with the page's input handed
// in. Coordinates are canvas (physical) pixels.
class WebGui : public Gui {
public:
   // The canvas size and the zoom the layout is drawn at.
   virtual void webSetFrame(int width, int height, double scale) = 0;
   // Repaints if anything changed; true when a new frame is ready.
   virtual bool webRender() = 0;
   // The last frame, RGBA, webFrameWidth() x webFrameHeight().
   virtual const uint8_t *webFrame() const = 0;
   virtual int webFrameWidth() const = 0;
   virtual int webFrameHeight() const = 0;

   virtual void webPointerDown(double x, double y, unsigned button, unsigned mods, double timeMs) = 0;
   virtual void webPointerUp(double x, double y, unsigned button, unsigned mods) = 0;
   virtual void webPointerMove(double x, double y, unsigned mods) = 0;
   virtual void webWheel(double x, double y, double notches, unsigned mods, double timeMs) = 0;
   virtual void webLeave() = 0;
   virtual bool webKey(unsigned keysym, const char *text, unsigned mods) = 0;
};

WebGui *createWebGui(GuiDelegate &delegate, CurveAccess &curves);
#endif

// The fork's own createWindow, declared in full so the fork's definition
// matches exactly. gui.cpp is its only caller.
Gui *createWindow(GuiDelegate &delegate, const plugincore::WindowSpec &spec,
                  CurveAccess &curves);

} // namespace shapeau
