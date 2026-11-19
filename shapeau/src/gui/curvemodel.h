#pragma once

// The editing rules behind CurveAccess: what an edit in the shape editor does
// to the shapes, the chain and the switches. The plugin is one CurveModel; the
// WCLAP's editor (web/uimain.cpp) is another, holding a mirror of the plugin's
// shapes and parameters and sending what it changes to the plugin. Keeping the
// rules here is what makes the two behave alike.
//
// Headless: no window code, so the plugin compiles it on a machine without
// Cairo.

#include <atomic>
#include <cstdint>

#include "gui/curveaccess.h"

namespace shapeau {

class CurveModel : public CurveAccess {
public:
   // The On switch of an effect.
   static uint32_t onParamForModule(int module);

   int curveModule() const override { return mCurveModule.load(); }
   int curveBand() const override { return mCurveBand.load(); }
   void curveSelectModule(int module) override;
   void curveSelectBand(int band) override;
   bool curveEditAllBands() const override { return mEditAllBands.load(); }
   void curveSetEditAllBands(bool all) override;

   const Curve &curveAt(int module, int band) const override;
   bool curveModuleEnabled(int module) const override;
   bool curveBandEnabled(int band) const override;
   void curveEnableModule(int module) override { ensureStageForEdit(module); }

   int curveAddPoint(int module, int band, float x, float y, int weight) override;
   void curveMovePoint(int module, int band, int index, float x, float y) override;
   void curveRemovePoint(int module, int band, int index) override;
   void curveSetWeight(int module, int band, int index, int weight, bool all) override;
   void curveSetFlat(int module, int band, float y) override;
   void curveSetBend(int module, int band, int index, float bend) override;
   void curveSetStep(int module, int band, int step, int steps, float y) override;
   void curveFillSteps(int module, int band, int steps) override;

   bool curveSnap() const override { return mSnap.load(); }
   void curveToggleSnap() override;
   bool curveStepMode() const override { return mStepMode.load(); }
   void curveSetStepMode(bool on) override;

   int curveChain(int *out) const override;
   void curveChainAdd(int module) override;
   void curveChainRemove(int module) override;
   void curveChainMove(int module, int place) override;
   void curveToggleModule(int module) override;

   int curveTriggerMode(int module) const override;
   bool curveTriggerLoop(int module) const override;
   float curveEnd(int module) const override;
   void curveBeginEnd(int module) override;
   void curveSetEnd(int module, float end) override;
   void curveFinishEnd(int module) override;

protected:
   // The parameters, as the holder keeps them: the real value of one, and the
   // three steps of an edit in the stored (raw) value.
   virtual double modelRealValue(uint32_t id) const = 0;
   virtual void modelBeginEdit(uint32_t id) = 0;
   virtual void modelSetParam(uint32_t id, double raw) = 0;
   virtual void modelEndEdit(uint32_t id) = 0;
   // The shapes or the chain in mCurves changed.
   virtual void modelCurvesChanged() = 0;
   // What the editor shows changed: the effect, the band, ALL, snap or step
   // mode. The plugin points its scope at it; the web editor tells the plugin.
   virtual void modelViewChanged() = 0;

   bool modelSwitched(uint32_t id) const;
   int modelEnum(uint32_t id) const;

   // Every listed effect, then the ones switched on without being listed,
   // written into the list itself. Every chain edit starts from it, so what
   // the bar showed is what the edit starts from.
   void materializeChain();

   // Drawing a shape into a stage that is switched off is a request to hear
   // it; the bands being edited are switched on with it.
   void ensureStageForEdit(int module);

   static int nearestPoint(const Curve &curve, float x);

   template <typename F> void forEditedBands(F apply) {
      if (mEditAllBands.load()) {
         for (int b = 0; b < kNumBands; ++b)
            apply(b);
      } else {
         apply(mCurveBand.load());
      }
   }

   CurveSet mCurves;
   std::atomic<int> mCurveModule{0};
   std::atomic<int> mCurveBand{1};
   // All three bands are edited by default: a shape on the mid band alone is
   // only heard by the mid band, which is not what drawing a volume stutter
   // across the graph looks like it should do.
   std::atomic<bool> mEditAllBands{true};
   std::atomic<bool> mStepMode{false};
   std::atomic<bool> mSnap{true};
};

} // namespace shapeau
