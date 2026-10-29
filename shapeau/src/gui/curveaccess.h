#pragma once

// The curve-editing interface between the plugin and its window, and the height
// of the editor strip. Kept apart from gui/gui.h because that header pulls in
// the window toolkit, which needs Cairo and X11; the plugin itself has to
// compile on a machine with neither (a headless build, or a Windows build with
// no Cairo cross-built yet).

#include "plugincore/gui/gui.h"

#include "dsp/curve.h"
#include "dsp/engine.h"

namespace shapeau {

// The shapes are plugin state rather than parameters: a host has no
// automation lane for a breakpoint list, and exposing 32 x-y pairs per band as
// 384 parameters would be a worse lie than admitting they are not automatable.
// So they are edited through here, stored in the plugin's state and preset
// files, and published to the audio thread the way any other edit is.
class CurveAccess {
public:
   virtual ~CurveAccess() = default;

   // Which module and band the editor is showing and editing. The stage's
   // controls stay on their own panels; this only decides what the graph draws
   // and where a click lands. When every band is being edited, `curveBand()`
   // is still the one drawn on top, so the graph has a shape to show.
   virtual int curveModule() const = 0;
   virtual int curveBand() const = 0;
   virtual void curveSelectModule(int module) = 0;
   virtual void curveSelectBand(int band) = 0;

   // ALL: edits go to all three bands at once. This is the default, because a
   // shape drawn across the mid band alone is a shape the low and high bands
   // do not hear -- which reads as the plugin doing half of what was drawn.
   virtual bool curveEditAllBands() const = 0;
   virtual void curveSetEditAllBands(bool all) = 0;

   // Every band's shape for the module on screen, so the editor can draw the
   // three over each other with the selected one on top.
   virtual const Curve &curveAt(int module, int band) const = 0;

   // Whether a stage or a band is switched on, for the editor's OFF marker.
   virtual bool curveModuleEnabled(int module) const = 0;
   virtual bool curveBandEnabled(int band) const = 0;

   // Turns a stage (and the bands being edited) on. The editor calls this when
   // a shape is drawn into a stage that is off, because drawing a shape is a
   // request to hear it; it is not called for a flat reset.
   virtual void curveEnableModule(int module) = 0;

   // Edits. Index is a curve point, as returned by Curve::count/point.
   /// Returns the index of the new point, or -1 when the curve is full, so the
   /// editor can start dragging it without having to search for it.
   virtual int curveAddPoint(int module, int band, float x, float y, int weight) = 0;
   virtual void curveMovePoint(int module, int band, int index, float x, float y) = 0;
   virtual void curveRemovePoint(int module, int band, int index) = 0;
   virtual void curveSetWeight(int module, int band, int index, int weight, bool all) = 0;
   virtual void curveSetFlat(int module, int band, float y) = 0;

   // The bend of the segment leaving point `index`, -1 .. +1. This is the
   // segment handle a DAW draws between two points.
   virtual void curveSetBend(int module, int band, int index, float bend) = 0;

   // Step editing: one cell of a `steps`-cell grid, or a conversion of the
   // whole shape to `steps` flat cells.
   virtual void curveSetStep(int module, int band, int step, int steps, float y) = 0;
   virtual void curveFillSteps(int module, int band, int steps) = 0;

   // Whether a dragged point snaps to the sixteenth grid. One setting for all
   // modules, which is what makes a shape drawn in one band line up with a
   // shape drawn in the next.
   virtual bool curveSnap() const = 0;
   virtual void curveToggleSnap() = 0;

   // The editor's step mode, so the window can persist it with the rest of its
   // state. It changes the mouse gesture, not the shape model.
   virtual bool curveStepMode() const = 0;
   virtual void curveSetStepMode(bool on) = 0;

   // The chain: the effects in it, in the order the audio runs through them,
   // into `out` (kNumModules long). Adding switches the effect on and selects
   // it; removing takes it out and switches it off; moving puts it at `place`
   // in the chain; toggling bypasses it and leaves it where it is.
   virtual int curveChain(int *out) const = 0;
   virtual void curveChainAdd(int module) = 0;
   virtual void curveChainRemove(int module) = 0;
   virtual void curveChainMove(int module, int place) = 0;
   virtual void curveToggleModule(int module) = 0;

   // The scope the audio thread writes for the effect and band on screen.
   virtual const Scope *curveScope() const = 0;

   // An effect's trigger, which the editor shows: what starts its cycle
   // (TriggerMode), whether a triggered cycle loops, and its End marker -- the
   // place a cycle run once holds at, drawn and dragged in the graph. The End
   // is a parameter, so a drag is a gesture the host can record.
   virtual int curveTriggerMode(int module) const = 0;
   virtual bool curveTriggerLoop(int module) const = 0;
   virtual float curveEnd(int module) const = 0;
   virtual void curveBeginEnd(int module) = 0;
   virtual void curveSetEnd(int module, float end) = 0;
   virtual void curveFinishEnd(int module) = 0;
};

// The height of the shape editor strip, in design pixels. The fork's layout
// and the height this plugin asks the host for both use it, so they cannot
// disagree about how tall the window is.
constexpr int kCurvePanelH = 440;

} // namespace shapeau
