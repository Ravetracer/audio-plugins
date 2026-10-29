#pragma once

// The shape editor: the one widget that is new in this fork of the shared
// window, kept in its own file so the fork's diff against shared/ is small and
// so the editing rules (hit tests, snapping, weights, bends, steps) are in one
// place rather than spread through the event handlers.
//
// It knows nothing about the plugin: it draws and edits through CurveAccess,
// and takes the colours it needs as a plain struct so it does not have to
// include the window's spec.

#include <cstdint>

#include <cairo/cairo.h>

#include "gui/gui.h"
#include "plugincore/gui/toolkit.h"

namespace shapeau {

struct CurvePalette {
   plugincore::Rgb accent;
   plugincore::Rgb panelFill;
   plugincore::Rgb panelEdge;
   plugincore::Rgb track;
   plugincore::Rgb text;
   plugincore::Rgb textDim;
   plugincore::Rgb textMute;
};

class CurvePanel {
public:
   void setRect(const plugincore::Rect &r) { mRect = r; }
   const plugincore::Rect &rect() const { return mRect; }
   bool contains(double x, double y) const { return mRect.contains(x, y); }

   void draw(cairo_t *cr, const CurvePalette &pal, const CurveAccess &access) const;

   // Mouse. press() returns true when the event belonged to this widget, in
   // which case the window must not look at it any further. motion() is only
   // called while a drag is running; release() ends one.
   bool press(double x, double y, unsigned button, double timeMs, CurveAccess &access);
   void motion(double x, double y, CurveAccess &access);
   void release(CurveAccess &access);
   // True while a point is dragged, a segment bent, steps painted or an effect
   // dragged along the bar, so the window keeps forwarding motion even once the
   // pointer has left the widget. Capture is the mouse's job.
   bool dragging() const {
      return mDragPoint >= 0 || mDragSegment >= 0 || mPaintStep || mTabDrag >= 0;
   }

   // The shape list and the list of effects to add are drawn over the graph. A
   // click outside the panel has to close them before it reaches a knob.
   bool menuOpen() const { return mShapeMenuOpen || mAddMenuOpen; }
   void closeMenu() {
      mShapeMenuOpen = false;
      mAddMenuOpen = false;
   }

   // Keeps the editor on an effect that is in the chain. Called by the window
   // before it paints; the chain can change under it from a preset or a host.
   void sync(CurveAccess &access);

private:
   plugincore::Rect graphRect() const;
   plugincore::Rect moduleTab(int order) const;
   plugincore::Rect addTab(int chainCount) const;
   plugincore::Rect addItemRect(int index, int chainCount) const;
   plugincore::Rect pickerRect(int index) const;
   void drawScope(cairo_t *cr, const CurvePalette &pal, const CurveAccess &access) const;
   void drawPicker(cairo_t *cr, const CurvePalette &pal) const;
   plugincore::Rect bandTab(int slot) const;
   plugincore::Rect snapRect() const;
   plugincore::Rect stepRect() const;
   plugincore::Rect shapeRect() const;
   plugincore::Rect shapeItemRect(int index) const;

   double toPhase(double x) const;
   double toValue(double y) const;
   double xOf(double phase) const;
   double yOf(double value) const;
   int pointAt(double x, double y, const Curve &curve) const;
   int segmentAt(double x, double y, const Curve &curve) const;
   int stepAt(double x) const;

   void applyShape(int index, double timeMs, CurveAccess &access);
   void paintStep(int index, double value, CurveAccess &access);

   plugincore::Rect mRect;
   int mDragPoint = -1;
   int mDragSegment = -1;
   int mDragModule = -1;
   int mDragBand = -1;
   float mDragBendStart = 0.0f;
   double mDragStartValue = 0.0;
   bool mPaintStep = false;
   int mLastPaintedStep = -1;
   double mLastClickTime = -1.0;
   int mLastClickPoint = -1;
   int mHoverPoint = -1;
   int mHoverSegment = -1;
   bool mShapeMenuOpen = false;
   int mShapeHover = -1;

   // The effect bar: the tab under the pointer, an effect being dragged to
   // another place in the chain, and the list of effects not in it yet.
   int mTabHover = -1;
   int mTabDrag = -1;
   double mTabDragStartX = 0.0;
   double mTabDragX = 0.0;
   bool mAddMenuOpen = false;
   int mAddHover = -1;
   int mPickerHover = -1;
};

} // namespace shapeau
