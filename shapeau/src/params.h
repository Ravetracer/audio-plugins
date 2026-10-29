#pragma once

#include <cstdint>

#include "plugincore/params.h"

namespace shapeau {

// The parameter model -- ParamDesc, ParamKind, the range mapping and the text
// formatting -- comes from the PluginCore shared library. Scoped to this
// namespace so nothing escapes into the global one.
using namespace plugincore;

// Parameter identifiers. These are persisted in preset files and plugin state,
// so the numeric values must never change: append new parameters at the end and
// never reorder or reuse an id.
enum ParamId : uint32_t {
   // ---------------------------------------------------------------- global
   kParamMix = 0,
   kParamInput,
   kParamOutput,
   kParamSplitLow,  // low band / mid band crossover
   kParamSplitHigh, // mid band / high band crossover
   kParamBandLow,   // Off/On, one per band
   kParamBandMid,
   kParamBandHigh,

   // ---------------------------------------------------------------- volume
   kParamVolOn,
   kParamVolTrim,
   kParamVolRate,
   kParamVolPhase,
   kParamVolSmooth,

   // ---------------------------------------------------------------- filter
   kParamFltOn,
   kParamFltCutoff,
   kParamFltRes,
   kParamFltDepth,
   kParamFltDrive,
   kParamFltTypeLow,
   kParamFltTypeMid,
   kParamFltTypeHigh,
   kParamFltRate,
   kParamFltPhase,
   kParamFltSmooth,

   // ----------------------------------------------------------------- drive
   kParamDrvOn,
   kParamDrvStyle,
   kParamDrvDrive,
   kParamDrvInput,
   kParamDrvOutput,
   kParamDrvTone,
   kParamDrvMix,
   kParamDrvRate,
   kParamDrvPhase,
   kParamDrvSmooth,

   // ------------------------------------------------------------------ time
   kParamTimOn,
   kParamTimRange,
   kParamTimStep, // how hard a jump in the read head is smoothed over
   kParamTimRate,
   kParamTimPhase,
   kParamTimSmooth,

   // ----------------------------------------------------------------- crush
   kParamCrsOn,
   kParamCrsAmount, // the top of the curve, below Bits and Down
   kParamCrsBits,
   kParamCrsDown, // sample-rate reduction, as a division of the sample rate
   kParamCrsMix,
   kParamCrsRate,
   kParamCrsPhase,
   kParamCrsSmooth,

   // ------------------------------------------------------------------- pan
   kParamPanOn,
   kParamPanRate,
   kParamPanPhase,
   kParamPanSmooth,

   // ----------------------------------------------------------------- width
   kParamWidOn,
   kParamWidRate,
   kParamWidPhase,
   kParamWidSmooth,

   kNumParams
};

// The stages, in the order they are processed. Also the order the shape curves
// are stored in: kNumModules * kNumBands of them. New modules are appended so
// the first four indices -- and therefore preset and state data written before
// they existed -- keep their meaning. Crush runs before Time in the audio path
// even though its tab sits after it in this list (see kAudioModules in
// curveaccess.h); the enum is storage order, not signal order.
enum Module {
   kModuleVolume = 0,
   kModuleFilter,
   kModuleDrive,
   kModuleTime,
   kModuleCrush,
   kModulePan,
   kModuleWidth,
   kNumModules
};

// The three bands. Low and mid are split at kParamSplitLow, mid and high at
// kParamSplitHigh; each band has its own curve for every module.
enum Band { kBandLow = 0, kBandMid, kBandHigh, kNumBands };

// The note values a module's curve can loop over. A beat is a quarter note, so
// the value each one means in beats is beatsForRate() below.
enum Rate {
   kRate32 = 0,
   kRate16,
   kRate8,
   kRate4,
   kRate2,
   kRate1,
   kRate2Bars,
   kRate4Bars,
   kNumRates
};

double beatsForRate(int rate);

// Filter responses. The values match plugincore::FilterKind, which is what the
// shared SVF switches on.
enum FilterType {
   kLowpass = 0,
   kHighpass,
   kBandpass,
   kNotch,
   kNumFilterTypes
};

// Drive transfer curves. A preset stores the *name*, so this list may be
// appended to but never reordered.
enum DriveStyle {
   kSoftClip = 0,
   kHardClip,
   kFold,
   kRectify,
   kSineFold,
   kCrush,
   kNumDriveStyles
};

// The plugin's own table, and lookups into it.
const ParamDesc *paramTable();
const ParamDesc *paramById(uint32_t id);
const ParamDesc *paramByKey(const char *key);

} // namespace shapeau
