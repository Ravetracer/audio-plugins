#pragma once

#include <cstdint>
#include <string>

#include "params.h"

namespace shapeau {

// One drawable shape: a list of breakpoints in normalised time and value.
//
// The curve is what a module is driven by, and it is deliberately plain: points
// in [0, 1] on both axes, each with a weight saying how hard its angle is and a
// bend saying how the segment leaving it curves. That makes it storable in a
// preset as text, evaluable per sample, and editable by a mouse without either
// side knowing about the other.
//
// Two points always exist at x = 0 and x = 1, so the shape covers exactly one
// cycle of the module's loop whatever it is; editing moves them vertically only.
// Points are kept sorted by x, which is what lets valueAt() be a short scan.
struct CurvePoint {
   float x = 0.0f;
   float y = 1.0f;
   // 0 hard (a corner), 1 medium (a rounded corner), 2 soft (a flowing curve).
   // The segment between two points uses the harder of the two weights, so one
   // soft point smooths both of its sides.
   uint8_t weight = 0;
   // How the segment that leaves this point bows, -1 .. +1. Zero is a straight
   // line; positive bows it towards the value axis' top (the segment rises
   // sooner and falls later), negative towards the bottom. It is the same
   // gesture as dragging a DAW's segment handle up or down, and it survives in
   // a preset as the fourth number of a point.
   float bend = 0.0f;
};

// A step shape needs two points at every cell boundary (one for the value
// before the jump, one for the value after), so sixteen steps cost 33 points.
// Sixty-four leaves room for a hand-drawn shape on top of a converted one.
constexpr int kMaxCurvePoints = 64;

class Curve {
public:
   Curve();

   // An empty curve is not a shape; a flat one is. This is what a module with
   // nothing drawn into it gets: a horizontal line at `y`.
   void setFlat(float y);

   int count() const { return mCount; }
   const CurvePoint &point(int i) const { return mPoints[i]; }

   // The value of the shape at `phase`, which is wrapped into [0, 1).
   // Interpolation follows the weights: hard is a straight line, medium
   // rounds the corner halfway, soft is a full smoothstep.
   float valueAt(float phase) const;

   // Inserts a point and returns its index, or the index of the point already
   // there if one is within a thousandth. Points are kept sorted. A new point
   // starts on a straight segment (bend 0).
   int addPoint(float x, float y, uint8_t weight);
   void removePoint(int i);
   void movePoint(int i, float x, float y);
   void setWeight(int i, uint8_t weight);
   void setBend(int i, float bend);

   // Step editing. setStep() paints one cell of a `steps`-cell grid: a flat
   // plateau at `y` with a hard edge at the cell's end. The last cell ends at
   // x = 1. fillSteps() converts the shape into `steps` flat cells, sampling
   // each cell's centre, which is what switching the editor into step mode
   // does. Both keep the curve in its normal point form, so a step shape is
   // still just a shape -- editable by points afterwards, and stored in a
   // preset like any other.
   void setStep(int step, int steps, float y);
   void fillSteps(int steps);

   // The text form used in preset files and in plugin state:
   //   "0.0000 1.0000 0 0.000, 0.2500 0.0000 2 0.400, ..."
   // That is time, value, corner weight and segment bend. A three-number group
   // -- the format before bend existed -- still loads, with a straight segment.
   // Parsing is tolerant of spacing and refuses a curve that could not be one
   // (fewer than two points, values outside [0, 1]) rather than repairing it
   // silently.
   std::string format() const;
   bool parse(const std::string &text);

private:
   void sortMiddle();
   void removeRange(float lo, float hi);

   CurvePoint mPoints[kMaxCurvePoints];
   int mCount = 2;
};

// Every shape: one for each module's each band, kNumModules * kNumBands of
// them. The engine reads a whole set, and the preset and state code write one.
//
// And the chain the shapes belong to: the effects that have been put in it, in
// the order the audio runs through them. It travels with the shapes because it
// is the same kind of thing -- state rather than parameters, edited in the
// window, carried in presets and handed to the audio thread in one piece.
//
// The list is not the whole story: an effect whose On switch is set runs even
// when it is not listed -- after the listed ones, in kDefaultChain order. That
// is what a preset written before the chain existed is (an empty list and some
// switches), and what a host automating a switch gets, so neither needs the
// window to have been opened first. effectiveChain() is the one place the two
// are put together.
struct CurveSet {
   Curve curve[kNumModules][kNumBands];
   int8_t chain[kNumModules] = {0, 0, 0, 0, 0, 0, 0};
   int8_t chainCount = 0;
};

// The order the effects ran in before the chain could be reordered.
constexpr int kDefaultChain[kNumModules] = {kModuleVolume, kModuleFilter, kModuleDrive,
                                            kModuleCrush,  kModuleTime,   kModuleWidth,
                                            kModulePan};

// The listed effects, then the unlisted ones that are switched on, by module
// index into `out` (kNumModules long). `on` is indexed by module. Returns how
// many there are.
int effectiveChain(const CurveSet &set, const bool *on, int *out);

// The chain's own edits. All of them keep the list free of duplicates and of
// anything that is not a module.
bool chainHas(const CurveSet &set, int module);
void chainAdd(CurveSet &set, int module);
void chainRemove(CurveSet &set, int module);
// Moves `module` to `place` in the chain, shifting the ones in between.
void chainMove(CurveSet &set, int module, int place);
// "vol flt drv" and back, for presets and state.
std::string formatChain(const CurveSet &set);
bool parseChain(const std::string &text, CurveSet &set);

// What a curve does when nothing has been drawn: each module's neutral shape.
// Volume is full gain, the filter is its base cutoff, drive and crush are their
// base amounts (zero does nothing), time is the present moment rather than the
// past, pan is the centre and width is the plain stereo image. A fresh plugin
// with every module switched off therefore passes audio untouched, and a
// module switched on over a neutral shape is transparent too.
float neutralValue(int module);
void resetCurves(CurveSet &set);

// The short key a module's curves are stored under: "vol", "flt", "drv",
// "tim", "crs", "pan", "wid". Appending a module is safe; changing a key
// would orphan the curve lines in every preset on disk.
const char *moduleKey(int module);
// The inverse; -1 when the text is not one of them.
int moduleFromKey(const std::string &key);

} // namespace shapeau
