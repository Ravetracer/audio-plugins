#include "gui/curvemodel.h"

#include <algorithm>
#include <cmath>

namespace shapeau {

uint32_t CurveModel::onParamForModule(int module) {
   switch (module) {
   case kModuleVolume:
      return kParamVolOn;
   case kModuleFilter:
      return kParamFltOn;
   case kModuleDrive:
      return kParamDrvOn;
   case kModuleTime:
      return kParamTimOn;
   case kModuleCrush:
      return kParamCrsOn;
   case kModulePan:
      return kParamPanOn;
   case kModuleWidth:
      return kParamWidOn;
   default:
      return kParamVolOn;
   }
}

bool CurveModel::modelSwitched(uint32_t id) const {
   return static_cast<int>(std::floor(modelRealValue(id) + 0.5)) != 0;
}

int CurveModel::modelEnum(uint32_t id) const {
   return static_cast<int>(std::floor(modelRealValue(id) + 0.5));
}

// ------------------------------------------------------------------ selection

void CurveModel::curveSelectModule(int module) {
   mCurveModule.store(std::clamp(module, 0, kNumModules - 1));
   modelViewChanged();
}

void CurveModel::curveSelectBand(int band) {
   mCurveBand.store(std::clamp(band, 0, kNumBands - 1));
   mEditAllBands.store(false);
   modelViewChanged();
}

void CurveModel::curveSetEditAllBands(bool all) {
   mEditAllBands.store(all);
   modelViewChanged();
}

void CurveModel::curveToggleSnap() {
   mSnap.store(!mSnap.load());
   modelViewChanged();
}

void CurveModel::curveSetStepMode(bool on) {
   mStepMode.store(on);
   modelViewChanged();
}

// ------------------------------------------------------------------ the chain
//
// The effects in the chain, in order: the listed ones, then any that are
// switched on without being listed (see CurveSet). Every edit first writes
// that whole order into the list, so what the bar showed is what the edit
// starts from, and then publishes it to the audio thread with the shapes.

int CurveModel::curveChain(int *out) const {
   bool on[kNumModules];
   for (int m = 0; m < kNumModules; ++m)
      on[m] = modelSwitched(onParamForModule(m));
   return effectiveChain(mCurves, on, out);
}

void CurveModel::materializeChain() {
   int order[kNumModules];
   const int n = curveChain(order);
   mCurves.chainCount = 0;
   for (int i = 0; i < n; ++i)
      chainAdd(mCurves, order[i]);
}

void CurveModel::curveChainAdd(int module) {
   materializeChain();
   chainAdd(mCurves, module);
   modelCurvesChanged();
   if (!modelSwitched(onParamForModule(module)))
      modelSetParam(onParamForModule(module), 1.0);
   curveSelectModule(module);
}

// Out of the list and switched off, or it would come back as an unlisted
// effect that is on. The editor moves to the effect that took its place.
void CurveModel::curveChainRemove(int module) {
   materializeChain();
   int at = 0;
   while (at < mCurves.chainCount && mCurves.chain[at] != module)
      ++at;
   chainRemove(mCurves, module);
   modelCurvesChanged();
   if (modelSwitched(onParamForModule(module)))
      modelSetParam(onParamForModule(module), 0.0);
   if (mCurves.chainCount > 0)
      curveSelectModule(mCurves.chain[std::min(at, mCurves.chainCount - 1)]);
}

void CurveModel::curveChainMove(int module, int place) {
   materializeChain();
   chainMove(mCurves, module, place);
   modelCurvesChanged();
}

// The power dot: bypass without leaving the chain. Listed first, so an
// effect switched off stays on the bar instead of vanishing from it.
void CurveModel::curveToggleModule(int module) {
   materializeChain();
   modelCurvesChanged();
   const uint32_t on = onParamForModule(module);
   modelSetParam(on, modelSwitched(on) ? 0.0 : 1.0);
}

// ----------------------------------------------------------------- the shapes

const Curve &CurveModel::curveAt(int module, int band) const {
   return mCurves.curve[std::clamp(module, 0, kNumModules - 1)][std::clamp(band, 0, kNumBands - 1)];
}

bool CurveModel::curveModuleEnabled(int module) const {
   return modelSwitched(onParamForModule(module));
}

bool CurveModel::curveBandEnabled(int band) const {
   return modelSwitched(kParamBandLow + std::clamp(band, 0, kNumBands - 1));
}

int CurveModel::curveAddPoint(int module, int band, float x, float y, int weight) {
   (void)band;
   ensureStageForEdit(module);
   int result = -1;
   forEditedBands([&](int b) {
      const int index = mCurves.curve[module][b].addPoint(x, y, static_cast<uint8_t>(weight));
      if (b == mCurveBand.load())
         result = index;
   });
   modelCurvesChanged();
   return result;
}

void CurveModel::curveMovePoint(int module, int band, int index, float x, float y) {
   (void)band;
   ensureStageForEdit(module);
   // In ALL mode the bands need not hold the same number of points (a loaded
   // preset can differ), so an index alone would move the wrong point in a
   // band that is shorter. The selected band's x is the anchor and each
   // other band moves its nearest point to it.
   const float anchor = mCurves.curve[module][mCurveBand.load()].point(index).x;
   forEditedBands([&](int b) {
      Curve &c = mCurves.curve[module][b];
      c.movePoint(b == mCurveBand.load() ? index : nearestPoint(c, anchor), x, y);
   });
   modelCurvesChanged();
}

void CurveModel::curveRemovePoint(int module, int band, int index) {
   (void)band;
   ensureStageForEdit(module);
   const Curve &selected = mCurves.curve[module][mCurveBand.load()];
   const float anchor = selected.point(index).x;
   forEditedBands([&](int b) {
      Curve &c = mCurves.curve[module][b];
      c.removePoint(b == mCurveBand.load() ? index : nearestPoint(c, anchor));
   });
   modelCurvesChanged();
}

void CurveModel::curveSetWeight(int module, int band, int index, int weight, bool all) {
   (void)band;
   ensureStageForEdit(module);
   const float anchor = mCurves.curve[module][mCurveBand.load()].point(index).x;
   forEditedBands([&](int b) {
      Curve &c = mCurves.curve[module][b];
      if (all) {
         for (int i = 0; i < c.count(); ++i)
            c.setWeight(i, static_cast<uint8_t>(weight));
      } else {
         c.setWeight(b == mCurveBand.load() ? index : nearestPoint(c, anchor),
                     static_cast<uint8_t>(weight));
      }
   });
   modelCurvesChanged();
}

void CurveModel::curveSetFlat(int module, int band, float y) {
   // A flat reset is not a request to hear the stage: switching a drive on
   // over a neutral curve would colour the sound the user just cleared.
   (void)band;
   forEditedBands([&](int b) { mCurves.curve[module][b].setFlat(y); });
   modelCurvesChanged();
}

void CurveModel::curveSetBend(int module, int band, int index, float bend) {
   (void)band;
   ensureStageForEdit(module);
   const float anchor = mCurves.curve[module][mCurveBand.load()].point(index).x;
   forEditedBands([&](int b) {
      Curve &c = mCurves.curve[module][b];
      c.setBend(b == mCurveBand.load() ? index : nearestPoint(c, anchor), bend);
   });
   modelCurvesChanged();
}

void CurveModel::curveSetStep(int module, int band, int step, int steps, float y) {
   (void)band;
   ensureStageForEdit(module);
   forEditedBands([&](int b) { mCurves.curve[module][b].setStep(step, steps, y); });
   modelCurvesChanged();
}

void CurveModel::curveFillSteps(int module, int band, int steps) {
   // Switching the editor into step mode converts the drawing; that is a
   // view change, not a reason to switch a stage on.
   (void)band;
   forEditedBands([&](int b) { mCurves.curve[module][b].fillSteps(steps); });
   modelCurvesChanged();
}

// ------------------------------------------------------------------- triggers

int CurveModel::curveTriggerMode(int module) const {
   return modelEnum(triggerParam(std::clamp(module, 0, kNumModules - 1)));
}

bool CurveModel::curveTriggerLoop(int module) const {
   return modelSwitched(loopParam(std::clamp(module, 0, kNumModules - 1)));
}

float CurveModel::curveEnd(int module) const {
   return static_cast<float>(modelRealValue(endParam(std::clamp(module, 0, kNumModules - 1))));
}

void CurveModel::curveBeginEnd(int module) {
   modelBeginEdit(endParam(std::clamp(module, 0, kNumModules - 1)));
}

void CurveModel::curveSetEnd(int module, float end) {
   modelSetParam(endParam(std::clamp(module, 0, kNumModules - 1)), end);
}

void CurveModel::curveFinishEnd(int module) {
   modelEndEdit(endParam(std::clamp(module, 0, kNumModules - 1)));
}

// -------------------------------------------------------------------- helpers

int CurveModel::nearestPoint(const Curve &curve, float x) {
   int best = 0;
   float distance = 2.0f;
   for (int i = 0; i < curve.count(); ++i) {
      const float d = std::fabs(curve.point(i).x - x);
      if (d < distance) {
         distance = d;
         best = i;
      }
   }
   return best;
}

// It also switches on the bands being edited, so a shape drawn on the low band
// while the low band is bypassed is not silent for a reason the drawing gives
// no clue about.
void CurveModel::ensureStageForEdit(int module) {
   const uint32_t on = onParamForModule(module);
   if (!modelSwitched(on))
      modelSetParam(on, 1.0);
   forEditedBands([&](int b) {
      const uint32_t bandOn = kParamBandLow + b;
      if (!modelSwitched(bandOn))
         modelSetParam(bandOn, 1.0);
   });
}

} // namespace shapeau
