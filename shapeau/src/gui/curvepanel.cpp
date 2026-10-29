#include "gui/curvepanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>

#include "gui/curveaccess.h"
#include "params.h"

namespace shapeau {

using plugincore::Align;
using plugincore::drawText;
using plugincore::Rect;
using plugincore::roundedRect;
using plugincore::setColor;
using plugincore::textWidth;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kToolbarH = 30.0;
constexpr double kGraphPad = 10.0;
constexpr double kLeftGutter = 34.0;
constexpr double kBottomGutter = 20.0;
constexpr double kModuleTabW = 86.0;
constexpr double kModuleTabGap = 3.0;
constexpr double kBandTabW = 48.0;
constexpr double kBandTabGap = 3.0;
constexpr double kPointRadius = 4.5;
constexpr double kHitRadius = 9.0;
constexpr int kStepCells = 16;
constexpr int kShapeCount = 12;

const char *const kModuleNames[kNumModules] = {"VOLUME", "FILTER", "DRIVE", "TIME",
                                               "CRUSH",  "PAN",    "WIDTH"};
const char *const kBandNames[kNumBands] = {"LOW", "MID", "HIGH"};
const char *const kShapeNames[kShapeCount] = {
   "FLAT",      "DUCK",  "GATE",     "RAMP UP", "RAMP DOWN", "TRIANGLE",
   "SQUARE",    "PULSE", "SINE",     "STAIRS",  "RANDOM",    "EXPO RISE"};

// A sixteenth of a cycle, which is the grid a shape drawn against a beat wants.
float snapPhase(float x) {
   if (x <= 0.0f)
      return 0.0f;
   if (x >= 1.0f)
      return 1.0f;
   return std::round(x * 16.0f) / 16.0f;
}

float clampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

Rect CurvePanel::graphRect() const {
   return {mRect.x + kLeftGutter + kGraphPad, mRect.y + kToolbarH + 8,
           mRect.w - kLeftGutter - 2 * kGraphPad,
           mRect.h - kToolbarH - kBottomGutter - 16};
}

Rect CurvePanel::moduleTab(int order) const {
   return {mRect.x + kGraphPad + order * (kModuleTabW + kModuleTabGap), mRect.y + 6, kModuleTabW,
           20};
}

// The tab after the last effect, which adds one. Narrow: it says "+".
Rect CurvePanel::addTab(int chainCount) const {
   const Rect t = moduleTab(chainCount);
   return {t.x, t.y, 28.0, t.h};
}

// The effects not in the chain yet, listed under +.
Rect CurvePanel::addItemRect(int index, int chainCount) const {
   const Rect a = addTab(chainCount);
   return {a.x, a.y + a.h + 4 + index * 20.0, 120, 19};
}

// The picker an empty chain shows in the graph: one button per effect.
Rect CurvePanel::pickerRect(int index) const {
   const Rect g = graphRect();
   constexpr double w = 120.0, h = 34.0, gap = 10.0;
   const double total = kNumModules * w + (kNumModules - 1) * gap;
   return {g.x + (g.w - total) * 0.5 + index * (w + gap), g.y + g.h * 0.5 - h * 0.5 + 10.0, w, h};
}

Rect CurvePanel::bandTab(int slot) const {
   const double x = mRect.x + kGraphPad + kNumModules * (kModuleTabW + kModuleTabGap) + 12.0;
   return {x + slot * (kBandTabW + kBandTabGap), mRect.y + 6, kBandTabW, 20};
}

Rect CurvePanel::snapRect() const {
   const double x = bandTab(kNumBands).x + kBandTabW + 12.0;
   return {x, mRect.y + 6, 54, 20};
}

Rect CurvePanel::stepRect() const { return {snapRect().x + 60.0, mRect.y + 6, 54, 20}; }

Rect CurvePanel::shapeRect() const { return {stepRect().x + 60.0, mRect.y + 6, 100, 20}; }

Rect CurvePanel::shapeItemRect(int index) const {
   const Rect s = shapeRect();
   const int col = index / 6;
   const int row = index % 6;
   return {s.x + col * 116.0, s.y + s.h + 4 + row * 18.0, 112, 17};
}

double CurvePanel::toPhase(double x) const {
   const Rect g = graphRect();
   return std::min(1.0, std::max(0.0, (x - g.x) / g.w));
}
double CurvePanel::toValue(double y) const {
   const Rect g = graphRect();
   return std::min(1.0, std::max(0.0, 1.0 - (y - g.y) / g.h));
}
double CurvePanel::xOf(double phase) const { return graphRect().x + phase * graphRect().w; }
double CurvePanel::yOf(double value) const {
   return graphRect().y + (1.0 - value) * graphRect().h;
}

int CurvePanel::pointAt(double x, double y, const Curve &curve) const {
   const double r2 = kHitRadius * kHitRadius;
   for (int i = 0; i < curve.count(); ++i) {
      const double px = xOf(curve.point(i).x);
      const double py = yOf(curve.point(i).y);
      if ((px - x) * (px - x) + (py - y) * (py - y) <= r2)
         return i;
   }
   return -1;
}

int CurvePanel::segmentAt(double x, double y, const Curve &curve) const {
   const Rect g = graphRect();
   if (x < g.x || x > g.x + g.w)
      return -1;
   // A point owns its neighbourhood; the segment starts beyond it.
   for (int i = 0; i < curve.count(); ++i)
      if (std::fabs(xOf(curve.point(i).x) - x) < 7.0)
         return -1;

   const double phase = toPhase(x);
   for (int i = 0; i + 1 < curve.count(); ++i) {
      const float a = curve.point(i).x;
      const float b = curve.point(i + 1).x;
      // A step's near-vertical edge is not a segment to bend.
      if (phase <= a || phase >= b || b - a < 0.02f)
         continue;
      if (std::fabs(yOf(curve.valueAt(static_cast<float>(phase))) - y) <= 8.0)
         return i;
   }
   return -1;
}

int CurvePanel::stepAt(double x) const {
   const int cell = static_cast<int>(std::floor(toPhase(x) * kStepCells));
   return std::max(0, std::min(cell, kStepCells - 1));
}

void CurvePanel::draw(cairo_t *cr, const CurvePalette &pal, const CurveAccess &access) const {
   const Rect &r = mRect;
   if (r.w <= 1 || r.h <= 1)
      return;

   setColor(cr, pal.panelFill);
   roundedRect(cr, r.x, r.y, r.w, r.h, 6);
   cairo_fill_preserve(cr);
   setColor(cr, pal.panelEdge);
   cairo_set_line_width(cr, 1.0);
   cairo_stroke(cr);

   // ------------------------------------------------------------- toolbar
   const int module = access.curveModule();
   const int band = access.curveBand();
   const bool allBands = access.curveEditAllBands();

   // The effect bar: the chain, in the order the audio runs through it. Each
   // tab has a power dot (bypass, without leaving the chain), the effect's name
   // (select, or drag to another place in the chain) and, under the pointer
   // or when selected, a cross that takes the effect out.
   int chain[kNumModules];
   const int chainCount = access.curveChain(chain);
   for (int order = 0; order < chainCount; ++order) {
      const int m = chain[order];
      Rect t = moduleTab(order);
      const bool dragged = m == mTabDrag;
      if (dragged)
         t.x += mTabDragX - mTabDragStartX;
      const bool selected = m == module;
      const bool on = access.curveModuleEnabled(m);
      setColor(cr, selected ? pal.accent : pal.track, selected ? 0.22 : (dragged ? 0.25 : 0.10));
      roundedRect(cr, t.x, t.y, t.w, t.h, 4);
      cairo_fill(cr);
      setColor(cr, on ? pal.accent : pal.textMute);
      cairo_arc(cr, t.x + 10, t.y + t.h * 0.5, 3.2, 0.0, 2.0 * kPi);
      if (on)
         cairo_fill(cr);
      else {
         cairo_set_line_width(cr, 1.2);
         cairo_stroke(cr);
      }
      setColor(cr, selected ? pal.accent : (on ? pal.textDim : pal.textMute));
      drawText(cr, t.x + t.w * 0.5 + 3, t.y + 14, kModuleNames[m], 8.5, true, Align::Center);
      if (m == mTabHover || selected) {
         const double cx = t.x + t.w - 9, cy = t.y + t.h * 0.5;
         setColor(cr, m == mTabHover ? pal.text : pal.textMute);
         cairo_set_line_width(cr, 1.3);
         cairo_move_to(cr, cx - 3, cy - 3);
         cairo_line_to(cr, cx + 3, cy + 3);
         cairo_move_to(cr, cx + 3, cy - 3);
         cairo_line_to(cr, cx - 3, cy + 3);
         cairo_stroke(cr);
      }
   }
   if (chainCount < kNumModules) {
      const Rect a = addTab(chainCount);
      setColor(cr, mAddMenuOpen ? pal.accent : pal.track, mAddMenuOpen ? 0.22 : 0.10);
      roundedRect(cr, a.x, a.y, a.w, a.h, 4);
      cairo_fill(cr);
      setColor(cr, mAddMenuOpen ? pal.accent : pal.textDim);
      cairo_set_line_width(cr, 1.6);
      cairo_move_to(cr, a.x + a.w * 0.5 - 5, a.y + a.h * 0.5);
      cairo_line_to(cr, a.x + a.w * 0.5 + 5, a.y + a.h * 0.5);
      cairo_move_to(cr, a.x + a.w * 0.5, a.y + a.h * 0.5 - 5);
      cairo_line_to(cr, a.x + a.w * 0.5, a.y + a.h * 0.5 + 5);
      cairo_stroke(cr);
   }

   for (int slot = 0; slot <= kNumBands; ++slot) {
      const Rect t = bandTab(slot);
      const bool selected = slot == 0 ? allBands : (!allBands && band == slot - 1);
      setColor(cr, selected ? pal.accent : pal.track, selected ? 0.22 : 0.10);
      roundedRect(cr, t.x, t.y, t.w, t.h, 4);
      cairo_fill(cr);
      setColor(cr, selected ? pal.accent : pal.textDim);
      drawText(cr, t.x + t.w * 0.5, t.y + 14, slot == 0 ? "ALL" : kBandNames[slot - 1], 8.5, true,
               Align::Center);
      if (slot > 0 && !access.curveBandEnabled(slot - 1)) {
         setColor(cr, pal.textMute);
         cairo_arc(cr, t.x + 6, t.y + 5, 2.0, 0.0, 2.0 * kPi);
         cairo_fill(cr);
      }
   }

   {
      const Rect s = snapRect();
      setColor(cr, access.curveSnap() ? pal.accent : pal.track,
               access.curveSnap() ? 0.22 : 0.10);
      roundedRect(cr, s.x, s.y, s.w, s.h, 4);
      cairo_fill(cr);
      setColor(cr, access.curveSnap() ? pal.accent : pal.textDim);
      drawText(cr, s.x + s.w * 0.5, s.y + 14, "SNAP", 8.5, true, Align::Center);
   }

   {
      const Rect s = stepRect();
      setColor(cr, access.curveStepMode() ? pal.accent : pal.track,
               access.curveStepMode() ? 0.22 : 0.10);
      roundedRect(cr, s.x, s.y, s.w, s.h, 4);
      cairo_fill(cr);
      setColor(cr, access.curveStepMode() ? pal.accent : pal.textDim);
      drawText(cr, s.x + s.w * 0.5, s.y + 14, "STEP", 8.5, true, Align::Center);
   }

   {
      const Rect s = shapeRect();
      setColor(cr, mShapeMenuOpen ? pal.accent : pal.track, mShapeMenuOpen ? 0.22 : 0.10);
      roundedRect(cr, s.x, s.y, s.w, s.h, 4);
      cairo_fill(cr);
      setColor(cr, mShapeMenuOpen ? pal.accent : pal.textDim);
      drawText(cr, s.x + s.w * 0.5 - 5, s.y + 14, "SHAPE", 8.5, true, Align::Center);
      // A small chevron, because a label alone does not say "this opens a list".
      cairo_set_line_width(cr, 1.4);
      cairo_move_to(cr, s.x + s.w - 17, s.y + 9);
      cairo_line_to(cr, s.x + s.w - 13, s.y + 13);
      cairo_line_to(cr, s.x + s.w - 9, s.y + 9);
      cairo_stroke(cr);
   }

   // --------------------------------------------------------------- graph
   const Rect g = graphRect();
   setColor(cr, pal.panelEdge, 0.30);
   cairo_rectangle(cr, g.x, g.y, g.w, g.h);
   cairo_fill(cr);

   if (chainCount == 0) {
      drawPicker(cr, pal);
      return;
   }
   drawScope(cr, pal, access);

   // The grid: sixteenths across, quarters up. In step mode every cell is also
   // a click target, which the sixteen divisions already say.
   cairo_set_line_width(cr, 1.0);
   for (int i = 1; i < kStepCells; ++i) {
      setColor(cr, pal.track, i % 4 == 0 ? 0.30 : 0.13);
      const double x = xOf(static_cast<double>(i) / kStepCells);
      cairo_move_to(cr, std::floor(x) + 0.5, g.y);
      cairo_line_to(cr, std::floor(x) + 0.5, g.y + g.h);
      cairo_stroke(cr);
   }
   for (int i = 1; i < 4; ++i) {
      setColor(cr, pal.track, 0.22);
      const double y = yOf(i / 4.0);
      cairo_move_to(cr, g.x, std::floor(y) + 0.5);
      cairo_line_to(cr, g.x + g.w, std::floor(y) + 0.5);
      cairo_stroke(cr);
   }

   setColor(cr, pal.textMute);
   drawText(cr, g.x - 6, g.y + 4, "100", 8.0, false, Align::Right);
   drawText(cr, g.x - 6, yOf(0.5) + 3, "50", 8.0, false, Align::Right);
   drawText(cr, g.x - 6, g.y + g.h + 3, "0", 8.0, false, Align::Right);
   drawText(cr, g.x, g.y + g.h + 16, "0", 8.0, false, Align::Left);
   drawText(cr, g.x + g.w * 0.5, g.y + g.h + 16, "1/2", 8.0, false, Align::Center);
   drawText(cr, g.x + g.w, g.y + g.h + 16, "1 cycle", 8.0, false, Align::Right);

   if (allBands) {
      // Under the top edge rather than on it: a curve resting at the top of
      // the editor owns that corner, and its endpoint is where the label was.
      setColor(cr, pal.accent, 0.75);
      drawText(cr, g.x + g.w - 8, g.y + 24, "ALL BANDS", 8.0, true, Align::Right);
   }

   // Three bands over each other: the selected one bright and thick, the other
   // two dim so the shape being edited is always the one that reads.
   for (int b = 0; b < kNumBands; ++b) {
      const Curve &curve = access.curveAt(module, b);
      const bool selected = b == band;
      setColor(cr, selected ? pal.accent : pal.textMute, selected ? 1.0 : 0.45);
      cairo_set_line_width(cr, selected ? 2.2 : 1.1);
      constexpr int kSteps = 400;
      for (int i = 0; i <= kSteps; ++i) {
         const double phase = static_cast<double>(i) / kSteps;
         const double x = xOf(phase);
         const double y = yOf(curve.valueAt(static_cast<float>(phase)));
         if (i == 0)
            cairo_move_to(cr, x, y);
         else
            cairo_line_to(cr, x, y);
      }
      cairo_stroke(cr);
   }

   // Points of the selected curve.
   const Curve &selected = access.curveAt(module, band);
   for (int i = 0; i < selected.count(); ++i) {
      const double x = xOf(selected.point(i).x);
      const double y = yOf(selected.point(i).y);
      const bool hot = i == mHoverPoint || (mDragPoint == i && mDragBand == band &&
                                            mDragModule == module);
      cairo_arc(cr, x, y, hot ? kPointRadius + 1.5 : kPointRadius, 0.0, 2.0 * kPi);
      setColor(cr, pal.panelFill);
      cairo_fill_preserve(cr);
      setColor(cr, hot ? pal.text : pal.accent);
      cairo_set_line_width(cr, 1.6);
      cairo_stroke(cr);
   }

   // The bend handle sits on the segment it shapes, halfway along it. Dragging
   // it up bows the segment towards the top of the editor and down towards the
   // bottom -- the same gesture as a DAW's segment handle.
   const int bent = mDragSegment >= 0 ? mDragSegment : mHoverSegment;
   if (bent >= 0 && bent + 1 < selected.count()) {
      const CurvePoint &a = selected.point(bent);
      const CurvePoint &b = selected.point(bent + 1);
      const double phase = 0.5 * (static_cast<double>(a.x) + b.x);
      const double x = xOf(phase);
      const double y = yOf(selected.valueAt(static_cast<float>(phase)));
      cairo_arc(cr, x, y, 4.5, 0.0, 2.0 * kPi);
      setColor(cr, pal.text);
      cairo_fill_preserve(cr);
      setColor(cr, pal.accent);
      cairo_set_line_width(cr, 1.4);
      cairo_stroke(cr);
   }

   // The editing rules, on their own backing so they stay readable wherever a
   // shape happens to be: the window's help line is for parameters, and there
   // is nothing else here to say how the editor works.
   const char *hint =
      access.curveStepMode()
         ? "STEP -- click or drag a cell to set its level; SHAPE for a starting point"
         : "click adds a point, drag moves it, double-click deletes, right-click its "
           "corner, drag the line between two points to bend it";
   const double hintW = std::min(g.w - 12.0, textWidth(cr, hint, 8.5, false) + 12.0);
   setColor(cr, pal.panelFill, 0.88);
   roundedRect(cr, g.x + 6, g.y + g.h - 22, hintW, 16, 4);
   cairo_fill(cr);
   setColor(cr, pal.textMute);
   drawText(cr, g.x + 12, g.y + g.h - 10, hint, 8.5, false, Align::Left);

   // The lists are drawn last, over the graph.
   if (mAddMenuOpen) {
      int n = 0;
      for (int m : kDefaultChain) {
         bool in = false;
         for (int i = 0; i < chainCount; ++i)
            in = in || chain[i] == m;
         if (in)
            continue;
         const Rect item = addItemRect(n, chainCount);
         setColor(cr, pal.panelFill, 0.98);
         cairo_rectangle(cr, item.x - 4, item.y - 2, item.w + 8, item.h + 4);
         cairo_fill(cr);
         if (n == mAddHover) {
            setColor(cr, pal.accent, 0.22);
            roundedRect(cr, item.x, item.y, item.w, item.h, 3);
            cairo_fill(cr);
         }
         setColor(cr, n == mAddHover ? pal.text : pal.textDim);
         drawText(cr, item.x + 8, item.y + 13, kModuleNames[m], 8.5, true, Align::Left);
         ++n;
      }
   }

   if (mShapeMenuOpen) {
      const Rect first = shapeItemRect(0);
      const Rect last = shapeItemRect(kShapeCount - 1);
      setColor(cr, pal.panelFill, 0.98);
      roundedRect(cr, first.x - 4, first.y - 4, (last.x + last.w) - first.x + 8,
                  (last.y + last.h) - first.y + 8, 5);
      cairo_fill_preserve(cr);
      setColor(cr, pal.panelEdge);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);
      for (int i = 0; i < kShapeCount; ++i) {
         const Rect item = shapeItemRect(i);
         if (i == mShapeHover) {
            setColor(cr, pal.accent, 0.22);
            roundedRect(cr, item.x, item.y, item.w, item.h, 3);
            cairo_fill(cr);
         }
         setColor(cr, i == mShapeHover ? pal.text : pal.textDim);
         drawText(cr, item.x + 8, item.y + 12, kShapeNames[i], 8.5, false, Align::Left);
      }
   }
}

// The audio behind the shape: what went in, in grey, and what came out, in the
// accent, along one cycle of the selected effect, with the playhead and the
// modulation trace -- what the effect really did, after Smooth. Volume,
// filter, drive, crush and time are drawn as magnitude from the bottom, so
// a duck drawn down to the floor sits on the part of the waveform it empties;
// pan and width, which move the image rather than the level, as a waveform
// around the middle.
void CurvePanel::drawScope(cairo_t *cr, const CurvePalette &pal, const CurveAccess &access) const {
   const Scope *scope = access.curveScope();
   if (!scope)
      return;
   const Rect g = graphRect();
   const int module = access.curveModule();
   const bool centred = module == kModulePan || module == kModuleWidth;
   constexpr int n = Scope::kBins;
   auto xAt = [&](int i) { return g.x + (i + 0.5) * g.w / n; };
   // Full scale reaches the top; a centred view has half the height each way.
   auto yAt = [&](float v) {
      v = clampF(v, -1.0f, 1.0f);
      return centred ? g.y + g.h * 0.5 - v * g.h * 0.5 : g.y + g.h - v * g.h;
   };
   auto band = [&](const std::atomic<float> *lo, const std::atomic<float> *hi) {
      for (int i = 0; i < n; ++i) {
         float top = hi[i].load(std::memory_order_relaxed);
         if (!centred)
            top = std::max(std::fabs(top), std::fabs(lo[i].load(std::memory_order_relaxed)));
         i == 0 ? cairo_move_to(cr, xAt(i), yAt(top)) : cairo_line_to(cr, xAt(i), yAt(top));
      }
      for (int i = n - 1; i >= 0; --i)
         cairo_line_to(cr, xAt(i), centred ? yAt(lo[i].load(std::memory_order_relaxed)) : g.y + g.h);
      cairo_close_path(cr);
      cairo_fill(cr);
   };
   setColor(cr, pal.textDim, 0.20);
   band(scope->inLo, scope->inHi);
   setColor(cr, pal.accent, 0.28);
   band(scope->outLo, scope->outHi);

   // The trace, where the modulation leaves the drawn shape.
   setColor(cr, pal.text, 0.55);
   cairo_set_line_width(cr, 1.2);
   bool drawing = false;
   for (int i = 0; i < n; ++i) {
      const float t = scope->trace[i].load(std::memory_order_relaxed);
      if (t < 0.0f) {
         drawing = false;
         continue;
      }
      const double x = xAt(i), y = yOf(t);
      drawing ? cairo_line_to(cr, x, y) : cairo_move_to(cr, x, y);
      drawing = true;
   }
   cairo_stroke(cr);

   const float head = scope->playhead.load(std::memory_order_relaxed);
   if (head >= 0.0f) {
      setColor(cr, pal.accent, 0.8);
      cairo_set_line_width(cr, 1.0);
      const double x = std::floor(xOf(head)) + 0.5;
      cairo_move_to(cr, x, g.y);
      cairo_line_to(cr, x, g.y + g.h);
      cairo_stroke(cr);
   }
}

// What an empty chain shows instead of a shape: the effects there are, one
// click from being in the chain.
void CurvePanel::drawPicker(cairo_t *cr, const CurvePalette &pal) const {
   const Rect g = graphRect();
   setColor(cr, pal.textDim);
   drawText(cr, g.x + g.w * 0.5, pickerRect(0).y - 18, "ADD AN EFFECT", 10, true, Align::Center);
   for (int i = 0; i < kNumModules; ++i) {
      const Rect r = pickerRect(i);
      const bool hot = i == mPickerHover;
      setColor(cr, hot ? pal.accent : pal.track, hot ? 0.25 : 0.14);
      roundedRect(cr, r.x, r.y, r.w, r.h, 5);
      cairo_fill(cr);
      setColor(cr, hot ? pal.accent : pal.textDim);
      drawText(cr, r.x + r.w * 0.5, r.y + r.h * 0.5 + 4, kModuleNames[kDefaultChain[i]], 9.5, true,
               Align::Center);
   }
   setColor(cr, pal.textMute);
   drawText(cr, g.x + g.w * 0.5, pickerRect(0).y + pickerRect(0).h + 24,
            "effects run left to right; drag one along the bar to change the order", 8.5, false,
            Align::Center);
}

void CurvePanel::sync(CurveAccess &access) {
   int chain[kNumModules];
   const int n = access.curveChain(chain);
   if (n == 0)
      return;
   const int module = access.curveModule();
   for (int i = 0; i < n; ++i)
      if (chain[i] == module)
         return;
   access.curveSelectModule(chain[0]);
}

bool CurvePanel::press(double x, double y, unsigned button, double timeMs, CurveAccess &access) {
   if (!mRect.contains(x, y))
      return false;

   // The list is a pop-up: a click that is not one of its items dismisses it
   // rather than also landing on whatever was underneath.
   if (mShapeMenuOpen) {
      for (int i = 0; i < kShapeCount; ++i)
         if (shapeItemRect(i).contains(x, y)) {
            applyShape(i, timeMs, access);
            mShapeMenuOpen = false;
            return true;
         }
      mShapeMenuOpen = false;
      return true;
   }

   int chain[kNumModules];
   const int chainCount = access.curveChain(chain);

   if (mAddMenuOpen) {
      int n = 0;
      for (int m : kDefaultChain) {
         bool in = false;
         for (int i = 0; i < chainCount; ++i)
            in = in || chain[i] == m;
         if (in)
            continue;
         if (addItemRect(n, chainCount).contains(x, y)) {
            access.curveChainAdd(m);
            break;
         }
         ++n;
      }
      mAddMenuOpen = false;
      return true;
   }

   // Toolbar first, so a click on a tab is never mistaken for a point.
   for (int order = 0; order < chainCount; ++order) {
      const Rect t = moduleTab(order);
      if (!t.contains(x, y))
         continue;
      const int m = chain[order];
      if (button != 1)
         return true;
      if (x < t.x + 20) {
         access.curveToggleModule(m);
      } else if (x > t.x + t.w - 18 && (m == mTabHover || m == access.curveModule())) {
         access.curveChainRemove(m);
         mTabHover = -1;
      } else {
         access.curveSelectModule(m);
         mTabDrag = m;
         mTabDragStartX = mTabDragX = x;
      }
      return true;
   }
   if (chainCount < kNumModules && addTab(chainCount).contains(x, y)) {
      mAddMenuOpen = true;
      mAddHover = -1;
      return true;
   }
   if (chainCount == 0) {
      for (int i = 0; i < kNumModules; ++i)
         if (pickerRect(i).contains(x, y) && button == 1)
            access.curveChainAdd(kDefaultChain[i]);
      return true;
   }
   for (int slot = 0; slot <= kNumBands; ++slot)
      if (bandTab(slot).contains(x, y)) {
         if (slot == 0)
            access.curveSetEditAllBands(true);
         else
            access.curveSelectBand(slot - 1);
         return true;
      }
   if (snapRect().contains(x, y)) {
      access.curveToggleSnap();
      return true;
   }
   if (stepRect().contains(x, y)) {
      const bool on = !access.curveStepMode();
      if (on)
         access.curveFillSteps(access.curveModule(), access.curveBand(), kStepCells);
      access.curveSetStepMode(on);
      return true;
   }
   if (shapeRect().contains(x, y)) {
      mShapeMenuOpen = true;
      mShapeHover = -1;
      return true;
   }

   if (!graphRect().contains(x, y))
      return true; // a click in the panel's padding: swallowed, not passed on

   const int module = access.curveModule();
   const int band = access.curveBand();
   const Curve &curve = access.curveAt(module, band);
   const int hit = pointAt(x, y, curve);
   const bool doubleClick = hit >= 0 && hit == mLastClickPoint && timeMs - mLastClickTime < 400.0;

   if (button == 3) { // right: cycle a point's corner, or straighten a segment
      if (hit >= 0) {
         const int w = (curve.point(hit).weight + 1) % 3;
         access.curveSetWeight(module, band, hit, w, false);
      } else {
         const int segment = segmentAt(x, y, curve);
         if (segment >= 0)
            access.curveSetBend(module, band, segment, 0.0f);
      }
      mLastClickTime = timeMs;
      mLastClickPoint = hit;
      return true;
   }

   if (button != 1)
      return true;

   if (access.curveStepMode()) {
      mPaintStep = true;
      mDragModule = module;
      mDragBand = band;
      mLastPaintedStep = -1;
      paintStep(stepAt(x), toValue(y), access);
      return true;
   }

   if (hit >= 0 && doubleClick) {
      access.curveRemovePoint(module, band, hit);
      mDragPoint = -1;
      mLastClickTime = -1.0;
      mLastClickPoint = -1;
      return true;
   }

   if (hit >= 0) {
      mDragPoint = hit;
   } else {
      const int segment = segmentAt(x, y, curve);
      if (segment >= 0) {
         mDragSegment = segment;
         mDragBendStart = curve.point(segment).bend;
         mDragStartValue = toValue(y);
      } else {
         float px = static_cast<float>(toPhase(x));
         if (access.curveSnap())
            px = snapPhase(px);
         mDragPoint = access.curveAddPoint(module, band, px, static_cast<float>(toValue(y)), 1);
      }
   }
   mDragModule = module;
   mDragBand = band;
   mLastClickTime = timeMs;
   mLastClickPoint = mDragPoint;
   return true;
}

void CurvePanel::motion(double x, double y, CurveAccess &access) {
   if (mTabDrag >= 0) {
      mTabDragX = x;
      return;
   }
   int chain[kNumModules];
   const int chainCount = access.curveChain(chain);
   mTabHover = -1;
   for (int order = 0; order < chainCount; ++order)
      if (moduleTab(order).contains(x, y))
         mTabHover = chain[order];
   if (mAddMenuOpen) {
      mAddHover = -1;
      for (int i = 0; i < kNumModules - chainCount; ++i)
         if (addItemRect(i, chainCount).contains(x, y))
            mAddHover = i;
      return;
   }
   mPickerHover = -1;
   if (chainCount == 0)
      for (int i = 0; i < kNumModules; ++i)
         if (pickerRect(i).contains(x, y))
            mPickerHover = i;
   if (mPaintStep) {
      paintStep(stepAt(x), toValue(y), access);
      return;
   }
   if (mDragPoint >= 0) {
      float px = static_cast<float>(toPhase(x));
      if (access.curveSnap())
         px = snapPhase(px);
      access.curveMovePoint(mDragModule, mDragBand, mDragPoint, px,
                            static_cast<float>(toValue(y)));
      return;
   }
   if (mDragSegment >= 0) {
      const double delta = toValue(y) - mDragStartValue;
      access.curveSetBend(mDragModule, mDragBand, mDragSegment,
                          clampF(mDragBendStart + static_cast<float>(delta * 4.0), -1.0f, 1.0f));
      return;
   }
   if (mShapeMenuOpen) {
      mShapeHover = -1;
      for (int i = 0; i < kShapeCount; ++i)
         if (shapeItemRect(i).contains(x, y))
            mShapeHover = i;
      return;
   }
   if (graphRect().contains(x, y)) {
      mHoverPoint = pointAt(x, y, access.curveAt(access.curveModule(), access.curveBand()));
      mHoverSegment =
         mHoverPoint < 0
            ? segmentAt(x, y, access.curveAt(access.curveModule(), access.curveBand()))
            : -1;
   } else {
      mHoverPoint = -1;
      mHoverSegment = -1;
   }
}

void CurvePanel::release(CurveAccess &access) {
   // An effect let go somewhere else on the bar takes the place it was dropped
   // on; a click that barely moved is only a selection.
   if (mTabDrag >= 0) {
      if (std::fabs(mTabDragX - mTabDragStartX) > 6.0) {
         int chain[kNumModules];
         const int n = access.curveChain(chain);
         const double first = moduleTab(0).x;
         const double pitch = kModuleTabW + kModuleTabGap;
         const int place = static_cast<int>(std::floor((mTabDragX - first) / pitch));
         access.curveChainMove(mTabDrag, std::max(0, std::min(place, n - 1)));
      }
      mTabDrag = -1;
   }
   mDragPoint = -1;
   mDragSegment = -1;
   mDragModule = -1;
   mDragBand = -1;
   mPaintStep = false;
   mLastPaintedStep = -1;
}

void CurvePanel::paintStep(int index, double value, CurveAccess &access) {
   if (index < 0)
      return;
   const int module = access.curveModule();
   // A fast drag across the grid must not leave unpainted cells behind, so the
   // cells between the last one and this one are filled with the same level.
   if (mLastPaintedStep >= 0 && std::abs(index - mLastPaintedStep) > 1) {
      const int dir = index > mLastPaintedStep ? 1 : -1;
      for (int s = mLastPaintedStep + dir; s != index; s += dir)
         access.curveSetStep(module, access.curveBand(), s, kStepCells,
                             static_cast<float>(value));
   }
   access.curveSetStep(module, access.curveBand(), index, kStepCells, static_cast<float>(value));
   mLastPaintedStep = index;
}

void CurvePanel::applyShape(int index, double timeMs, CurveAccess &access) {
   const int module = access.curveModule();
   const int band = access.curveBand();
   auto flat = [&](float y) { access.curveSetFlat(module, band, y); };
   auto add = [&](float x, float y, int weight) {
      return access.curveAddPoint(module, band, x, y, weight);
   };

   // A generator other than the flat reset is a request to hear the stage.
   if (index != 0)
      access.curveEnableModule(module);

   switch (index) {
   case 0: // flat: the module's neutral shape
      flat(neutralValue(module));
      break;
   case 1: // duck: a dip at the start of the cycle, smooth either side
      flat(1.0f);
      add(0.0f, 0.05f, 2);
      add(0.16f, 1.0f, 2);
      break;
   case 2: // gate: open for the first 45 %, shut for the rest
      flat(0.0f);
      add(0.0f, 1.0f, 0);
      add(0.45f, 1.0f, 0);
      add(0.5f, 0.0f, 0);
      break;
   case 3: // ramp up
      flat(0.0f);
      add(1.0f, 1.0f, 0);
      break;
   case 4: // ramp down
      flat(1.0f);
      add(1.0f, 0.0f, 0);
      break;
   case 5: // triangle
      flat(0.0f);
      add(0.0f, 0.0f, 0);
      add(0.5f, 1.0f, 0);
      add(1.0f, 0.0f, 0);
      break;
   case 6: // square: half the cycle open, half shut
      flat(0.0f);
      add(0.0f, 1.0f, 0);
      add(0.498f, 1.0f, 0);
      add(0.5f, 0.0f, 0);
      break;
   case 7: // pulse: a short burst at the top of the cycle
      flat(0.0f);
      add(0.0f, 1.0f, 0);
      add(0.22f, 1.0f, 0);
      add(0.25f, 0.0f, 0);
      break;
   case 8: { // sine: a full tremolo, sampled finely enough to read as a curve
      flat(0.0f);
      for (int i = 0; i <= 16; ++i) {
         const float x = static_cast<float>(i) / 16.0f;
         const float y = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(kPi) * x);
         add(x, y, 2);
      }
      break;
   }
   case 9: { // stairs: eight descending steps
      flat(0.0f);
      for (int i = 0; i < 8; ++i)
         access.curveSetStep(module, band, i, 8, 1.0f - static_cast<float>(i) * 0.125f);
      break;
   }
   case 10: { // random: sixteen steps, a new draw every time it is chosen
      flat(0.0f);
      uint32_t seed = static_cast<uint32_t>(timeMs * 1000.0) ^ 0x9E3779B9u;
      if (seed == 0)
         seed = 1;
      auto next = [&]() {
         seed ^= seed << 13;
         seed ^= seed >> 17;
         seed ^= seed << 5;
         return static_cast<float>(seed & 0xFFFFFFu) / 16777215.0f;
      };
      for (int i = 0; i < kStepCells; ++i)
         access.curveSetStep(module, band, i, kStepCells, next());
      break;
   }
   case 11: // exponential rise, which is the bend handle in one click
      flat(0.0f);
      add(1.0f, 1.0f, 0);
      access.curveSetBend(module, band, 0, 0.85f);
      break;
   default:
      break;
   }
}

} // namespace shapeau
