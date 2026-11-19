#pragma once

// The window's WebAssembly backend, as the editor module (src/web/ui_main.cpp)
// drives it: the page's canvas, its pointer and its keys.
//
// There is no window system in a browser page. The window draws into its Cairo
// image surface exactly as on the other platforms, and the page copies each
// finished frame into a canvas; input arrives through the editor module's
// exports and is handed over here. Only built for WebAssembly.

#include <cstdint>

namespace plugincore {
class Gui;
}

namespace rumpelkiste {

class WebCanvas {
public:
   virtual ~WebCanvas() = default;

   // The page's size in CSS pixels and its devicePixelRatio. The window zooms
   // to the largest scale that fits and draws at the device's resolution.
   virtual void webSetPage(int cssWidth, int cssHeight, double devicePixelRatio) = 0;
   // The layout's own size at scale 1, which changes with the collapsible
   // section and the FX page.
   virtual void webDesignSize(uint32_t &width, uint32_t &height) const = 0;

   // Once per animation frame: repaints if anything changed. Returns the
   // serial of the newest frame.
   virtual uint32_t webTick() = 0;
   // The newest frame: RGBA, width x height x 4 bytes, the page's size.
   virtual const uint8_t *webFrame(int &width, int &height) const = 0;

   // Canvas pixels. Buttons 1 left, 2 middle, 3 right; mods 1 shift, 2 ctrl,
   // 4 alt.
   virtual void webPointerDown(double x, double y, int button, unsigned mods) = 0;
   virtual void webPointerUp(double x, double y, int button, unsigned mods) = 0;
   virtual void webPointerMove(double x, double y, unsigned mods) = 0;
   virtual void webWheel(double x, double y, double notches, unsigned mods) = 0;
   virtual void webLeave() = 0;
   // An X11 keysym and the key's text. Returns whether the window used it.
   virtual bool webKey(unsigned keysym, const char *text, int length, unsigned mods) = 0;
   // Whether a button is held, which is when an edit is still going on.
   virtual bool webButtonHeld() const = 0;
};

// The canvas behind a window made by createWindow().
WebCanvas *webCanvasOf(plugincore::Gui *gui);

} // namespace rumpelkiste
