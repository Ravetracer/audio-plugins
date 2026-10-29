#include "params.h"

#include <cmath>
#include <cstring>

#include "plugincore/param_macros.h"

namespace shapeau {

namespace {

const char *const kOnOff[] = {"Off", "On"};
const char *const kRateNames[] = {"1/32", "1/16", "1/8", "1/4",
                                  "1/2",  "1/1",  "2 Bars", "4 Bars"};
const char *const kFilterNames[] = {"Lowpass", "Highpass", "Bandpass", "Notch"};
// A preset stores the name, so this list may be appended to but never reordered.
const char *const kDriveNames[] = {"Soft Clip", "Hard Clip", "Fold",
                                   "Rectify",   "Sine Fold", "Crush"};
const char *const kTriggerNames[] = {"Sync", "MIDI", "Audio"};
const char *const kSourceNames[] = {"Sidechain", "Input"};
const char *const kNoteNames[] = {"Any", "C",  "C#", "D",  "D#", "E", "F",
                                  "F#",  "G",  "G#", "A",  "A#", "B"};
const char *const kOctaveNames[] = {"Any", "-1", "0", "1", "2", "3",
                                    "4",   "5",  "6", "7", "8", "9"};

// A log parameter's default is its position on the travel, not its value.
double logPos(double lo, double value, double hi) { return std::log(value / lo) / std::log(hi / lo); }

const ParamDesc kParams[kNumParams] = {
   // ---------------------------------------------------------------- global
   PCT(kParamMix, "mix", "Mix", "Global", 1.0,
       "The whole rack against the input, not one stage. A curve that ducks or "
       "stutters is most useful mixed in rather than in place."),
   LIN(kParamInput, "input", "Input", "Global", -24.0, 24.0, 0.0, "dB",
       "Gain into the rack, before the band split. The drive stage has its own, "
       "and this one is here for the rare source that needs it."),
   LIN(kParamOutput, "output", "Output", "Global", -24.0, 24.0, 0.0, "dB",
       "Gain out of the rack, after the bands are summed and mixed."),
   LOG(kParamSplitLow, "split_low", "Low Split", "Global", logPos(20.0, 200.0, 2000.0), 20.0,
       2000.0, "Hz",
       "Where the low band ends and the mid band begins. Both sides are "
       "Linkwitz-Riley fourth order, so the three bands sum back to the input."),
   LOG(kParamSplitHigh, "split_high", "High Split", "Global", logPos(200.0, 2000.0, 20000.0),
       200.0, 20000.0, "Hz",
       "Where the mid band ends and the high band begins. Each band carries its "
       "own curve for every stage, which is the point of the split."),
   ENUM(kParamBandLow, "band_low", "Low Band", "Global", 1, kOnOff,
        "Whether the low band is processed. Switched off, it passes through "
        "untouched and is still summed with the other two."),
   ENUM(kParamBandMid, "band_mid", "Mid Band", "Global", 1, kOnOff,
        "Whether the mid band is processed."),
   ENUM(kParamBandHigh, "band_high", "High Band", "Global", 1, kOnOff,
        "Whether the high band is processed."),

   // ---------------------------------------------------------------- volume
   ENUM(kParamVolOn, "vol_on", "Volume", "Volume", 0, kOnOff,
        "Draws the level: ducking, gating, tremolo. The curve is squared, so a "
        "point halfway down the editor is about -12 dB."),
   LIN(kParamVolTrim, "vol_trim", "Trim", "Volume", -24.0, 24.0, 0.0, "dB",
        "Gain on the volume stage's output, which is what lets a curve duck "
        "and still leave the result louder than it came in."),
   ENUM(kParamVolRate, "vol_rate", "Rate", "Volume", 3, kRateNames,
        "The length of one cycle of the curve, in note values locked to the "
        "host's tempo."),
   PCT(kParamVolPhase, "vol_phase", "Phase", "Volume", 0.0,
       "Where in the cycle the curve starts. A quarter-cycle shift moves a "
       "duck drawn on the beat to a sixteenth before it."),
   PCT(kParamVolSmooth, "vol_smooth", "Smooth", "Volume", 0.15,
       "Softens the corners of the drawn shape. At zero the curve is followed "
       "exactly, which is right for a gate and wrong for a duck."),

   // ---------------------------------------------------------------- filter
   ENUM(kParamFltOn, "flt_on", "Filter", "Filter", 0, kOnOff,
        "Draws the cutoff: a sweep, a wobble, a plucked envelope per band."),
   LOG(kParamFltCutoff, "flt_cutoff", "Cutoff", "Filter", logPos(20.0, 1000.0, 20000.0), 20.0,
       20000.0, "Hz",
       "The cutoff the curve's top reaches. With the curve drawn flat along "
       "the top of the editor this is simply the cutoff."),
   PCT(kParamFltRes, "flt_res", "Resonance", "Filter", 0.25,
       "Emphasis at the cutoff. Pushed high the shared SVF is close to "
       "self-oscillation, which the drive control in front of it can excite."),
   STEP(kParamFltDepth, "flt_depth", "Depth", "Filter", 0.0, 8.0, 5.0, "oct",
        "How far below the cutoff the bottom of the curve reaches, in octaves. "
        "Eight octaves puts the bottom of the editor under 20 Hz from any "
        "cutoff in the top octave; two gives a gentle breath."),
   PCT(kParamFltDrive, "flt_drive", "Drive", "Filter", 0.0,
       "Saturates the band before the filter. A resonant sweep that is driven "
       "excites its own harmonics instead of staying clean."),
   ENUM(kParamFltTypeLow, "flt_type_low", "Low Type", "Filter", 0, kFilterNames,
        "The response the low band is filtered with. Each band chooses its "
        "own, so the low band can be a lowpass while the high band is a notch."),
   ENUM(kParamFltTypeMid, "flt_type_mid", "Mid Type", "Filter", 0, kFilterNames,
        "The mid band's response."),
   ENUM(kParamFltTypeHigh, "flt_type_high", "High Type", "Filter", 0, kFilterNames,
        "The high band's response."),
   ENUM(kParamFltRate, "flt_rate", "Rate", "Filter", 3, kRateNames,
        "The length of one cycle of the cutoff curve."),
   PCT(kParamFltPhase, "flt_phase", "Phase", "Filter", 0.0,
       "Where in the cycle the cutoff curve starts."),
   PCT(kParamFltSmooth, "flt_smooth", "Smooth", "Filter", 0.25,
       "Rounds off the drawn steps. A filter with no smoothing on a hard "
       "step clicks, which is sometimes the sound wanted and sometimes not."),

   // ----------------------------------------------------------------- drive
   ENUM(kParamDrvOn, "drv_on", "Drive", "Drive", 0, kOnOff,
        "Draws the amount of distortion: a rhythmic grit, or saturation that "
        "only appears on the hits."),
   ENUM(kParamDrvStyle, "drv_style", "Style", "Drive", 0, kDriveNames,
        "Which transfer curve shapes the signal. One style is shared by all "
        "three bands; it is the amount that is drawn per band."),
   PCT(kParamDrvDrive, "drv_drive", "Drive", "Drive", 0.4,
       "The amount the curve is scaled from: the top of the curve is this "
       "much drive, the bottom is none."),
   LIN(kParamDrvInput, "drv_input", "Input", "Drive", -12.0, 36.0, 0.0, "dB",
        "Gain before the shaper, separate from the stage's own drive, so a "
        "quiet source can be put where the curve expects it."),
   LIN(kParamDrvOutput, "drv_output", "Output", "Drive", -36.0, 12.0, 0.0, "dB",
        "Gain after the shaper, for balancing the drive against everything "
        "else in the rack."),
   BIPCT(kParamDrvTone, "drv_tone", "Tone", "Drive", 0.0,
         "A one-pole at 800 Hz mixed in with the distortion: to the right "
         "takes the fizz off, to the left thins it out."),
   PCT(kParamDrvMix, "drv_mix", "Mix", "Drive", 1.0,
       "The distortion against the band's clean signal. Below 100 % this is "
       "the parallel blend, which is where a bass keeps its bottom."),
   ENUM(kParamDrvRate, "drv_rate", "Rate", "Drive", 3, kRateNames,
        "The length of one cycle of the drive curve."),
   PCT(kParamDrvPhase, "drv_phase", "Phase", "Drive", 0.0,
       "Where in the cycle the drive curve starts."),
   PCT(kParamDrvSmooth, "drv_smooth", "Smooth", "Drive", 0.2,
       "Softens the drive curve. A hard-stepped drive shape is a burst of "
       "distortion, which can click at its edges without this."),

   // ------------------------------------------------------------------ time
   ENUM(kParamTimOn, "tim_on", "Time", "Time", 0, kOnOff,
        "Scrubs through the last moment of the band: stutters, half-speed, "
        "reverse and tape stops, all drawn as the read head's position."),
   LOG(kParamTimRange, "tim_range", "Range", "Time", logPos(20.0, 500.0, 2000.0), 20.0, 2000.0,
       "ms",
       "The longest look-back. The bottom of the curve reaches this far into "
       "the past; the top is the present moment."),
   PCT(kParamTimStep, "tim_step", "Step", "Time", 0.3,
       "How long the read head takes to move when the curve jumps. It is the "
       "difference between a click at every step and a smooth stutter."),
   ENUM(kParamTimRate, "tim_rate", "Rate", "Time", 3, kRateNames,
        "The length of one cycle of the read-head curve. A stutter usually "
        "wants a sixteenth; a tape stop, a bar."),
   PCT(kParamTimPhase, "tim_phase", "Phase", "Time", 0.0,
       "Where in the cycle the read-head curve starts."),
   PCT(kParamTimSmooth, "tim_smooth", "Smooth", "Time", 0.1,
       "Softens the drawn shape before it moves the read head, on top of "
       "Step. Useful when a coarse grid is deliberate but the edges are not."),

   // ----------------------------------------------------------------- crush
   ENUM(kParamCrsOn, "crs_on", "Crush", "Crush", 0, kOnOff,
        "Draws the amount of bit and sample-rate reduction. The top of the "
        "curve is the Amount knob, the bottom is clean."),
   PCT(kParamCrsAmount, "crs_amount", "Amount", "Crush", 0.6,
       "The amount the curve scales from: the top of the curve is this much "
       "crush, the bottom is none."),
   STEP(kParamCrsBits, "crs_bits", "Bits", "Crush", 2.0, 16.0, 6.0, "bit",
        "The bit depth at full curve. Sixteen is effectively clean; the low "
        "end is a hard, grainy quantiser."),
   STEP(kParamCrsDown, "crs_down", "Down", "Crush", 1.0, 32.0, 8.0, "x",
        "The sample-rate division at full curve. At 8 the signal is held for "
        "one sample in eight, which is the aliased, lo-fi half of a crusher."),
   PCT(kParamCrsMix, "crs_mix", "Mix", "Crush", 1.0,
       "The crushed signal against the band's clean one. Below 100 % this is "
       "a parallel blend, which keeps a bass' weight under the grit."),
   ENUM(kParamCrsRate, "crs_rate", "Rate", "Crush", 3, kRateNames,
        "The length of one cycle of the crush curve."),
   PCT(kParamCrsPhase, "crs_phase", "Phase", "Crush", 0.0,
       "Where in the cycle the crush curve starts."),
   PCT(kParamCrsSmooth, "crs_smooth", "Smooth", "Crush", 0.1,
       "Rounds off the crush amount. A hard-stepped crush curve is a burst of "
       "grain, which can click at its edges without this."),

   // ------------------------------------------------------------------- pan
   ENUM(kParamPanOn, "pan_on", "Pan", "Pan", 0, kOnOff,
        "Draws each band's position between the speakers: the middle of the "
        "curve is the centre, the bottom is hard left and the top hard right."),
   ENUM(kParamPanRate, "pan_rate", "Rate", "Pan", 3, kRateNames,
        "The length of one cycle of the pan curve."),
   PCT(kParamPanPhase, "pan_phase", "Phase", "Pan", 0.0,
       "Where in the cycle the pan curve starts."),
   PCT(kParamPanSmooth, "pan_smooth", "Smooth", "Pan", 0.15,
       "Softens the pan movement. At zero a hard pan step is a jump; a little "
       "smoothing makes it a move."),

   // ----------------------------------------------------------------- width
   ENUM(kParamWidOn, "wid_on", "Width", "Width", 0, kOnOff,
        "Draws the stereo width per band: the middle of the curve is the plain "
        "image, the bottom is mono and the top is double width."),
   ENUM(kParamWidRate, "wid_rate", "Rate", "Width", 3, kRateNames,
        "The length of one cycle of the width curve."),
   PCT(kParamWidPhase, "wid_phase", "Phase", "Width", 0.0,
        "Where in the cycle the width curve starts."),
   PCT(kParamWidSmooth, "wid_smooth", "Smooth", "Width", 0.15,
        "Softens the width movement, which keeps a hard step from tearing the "
        "image apart."),

   // --------------------------------------------------------------- trigger
   ENUM(kParamTrigSource, "trig_source", "Source", "Trigger", 0, kSourceNames,
        "What an Audio trigger listens to: the sidechain input, where a kick "
        "track is routed to duck a bass, or the signal coming into the rack."),
   LIN(kParamTrigThreshold, "trig_threshold", "Threshold", "Trigger", -60.0, 0.0, -24.0, "dB",
       "How loud a hit must be to start an effect set to Audio. It re-arms once "
       "the level has fallen 6 dB below this."),
   LOG(kParamTrigFilter, "trig_filter", "Focus", "Trigger", 1.0, 40.0, 20000.0, "Hz",
       "A lowpass in front of the detector only, never the sound. Around 150 Hz "
       "it hears the kick in a drum loop and ignores the rest, a few ms later."),
   LOG(kParamTrigHold, "trig_hold", "Hold", "Trigger", logPos(5.0, 50.0, 1000.0), 5.0, 1000.0,
       "ms",
       "The shortest time between two audio triggers, so a kick with a long or "
       "doubled attack starts the effect once."),
   ENUM(kParamTrigNote, "trig_note", "Note", "Trigger", 0, kNoteNames,
        "Which note starts an effect set to MIDI. Any answers to every note; a "
        "note name answers to it in the octave chosen beside it."),
   ENUM(kParamTrigOctave, "trig_octave", "Octave", "Trigger", 0, kOctaveNames,
        "The octave of the trigger note, with C3 as MIDI note 48. Any lets the "
        "note name match in every octave."),
   ENUM(kParamVolTrigger, "vol_trigger", "Trigger", "Volume", 0, kTriggerNames,
        "What starts the volume cycle: the host's beat, a MIDI note, or a "
        "hit in the sidechain or the input, as the Trigger panel sets it up."),
   ENUM(kParamVolLoop, "vol_loop", "Loop", "Volume", 0, kOnOff,
        "With MIDI or Audio: On keeps cycling until the next trigger, Off runs "
        "the shape once and holds at its End marker."),
   PCT(kParamVolEnd, "vol_end", "End", "Volume", 1.0,
       "Where a shape run once stops and holds, until the next trigger. Drag "
       "the marker in the editor or turn this; Sync and Loop ignore it."),
   ENUM(kParamFltTrigger, "flt_trigger", "Trigger", "Filter", 0, kTriggerNames,
        "What starts the filter cycle: the host's beat, a MIDI note, or a "
        "hit in the sidechain or the input, as the Trigger panel sets it up."),
   ENUM(kParamFltLoop, "flt_loop", "Loop", "Filter", 0, kOnOff,
        "With MIDI or Audio: On keeps cycling until the next trigger, Off runs "
        "the shape once and holds at its End marker."),
   PCT(kParamFltEnd, "flt_end", "End", "Filter", 1.0,
       "Where a shape run once stops and holds, until the next trigger. Drag "
       "the marker in the editor or turn this; Sync and Loop ignore it."),
   ENUM(kParamDrvTrigger, "drv_trigger", "Trigger", "Drive", 0, kTriggerNames,
        "What starts the drive cycle: the host's beat, a MIDI note, or a "
        "hit in the sidechain or the input, as the Trigger panel sets it up."),
   ENUM(kParamDrvLoop, "drv_loop", "Loop", "Drive", 0, kOnOff,
        "With MIDI or Audio: On keeps cycling until the next trigger, Off runs "
        "the shape once and holds at its End marker."),
   PCT(kParamDrvEnd, "drv_end", "End", "Drive", 1.0,
       "Where a shape run once stops and holds, until the next trigger. Drag "
       "the marker in the editor or turn this; Sync and Loop ignore it."),
   ENUM(kParamTimTrigger, "tim_trigger", "Trigger", "Time", 0, kTriggerNames,
        "What starts the time cycle: the host's beat, a MIDI note, or a "
        "hit in the sidechain or the input, as the Trigger panel sets it up."),
   ENUM(kParamTimLoop, "tim_loop", "Loop", "Time", 0, kOnOff,
        "With MIDI or Audio: On keeps cycling until the next trigger, Off runs "
        "the shape once and holds at its End marker."),
   PCT(kParamTimEnd, "tim_end", "End", "Time", 1.0,
       "Where a shape run once stops and holds, until the next trigger. Drag "
       "the marker in the editor or turn this; Sync and Loop ignore it."),
   ENUM(kParamCrsTrigger, "crs_trigger", "Trigger", "Crush", 0, kTriggerNames,
        "What starts the crush cycle: the host's beat, a MIDI note, or a "
        "hit in the sidechain or the input, as the Trigger panel sets it up."),
   ENUM(kParamCrsLoop, "crs_loop", "Loop", "Crush", 0, kOnOff,
        "With MIDI or Audio: On keeps cycling until the next trigger, Off runs "
        "the shape once and holds at its End marker."),
   PCT(kParamCrsEnd, "crs_end", "End", "Crush", 1.0,
       "Where a shape run once stops and holds, until the next trigger. Drag "
       "the marker in the editor or turn this; Sync and Loop ignore it."),
   ENUM(kParamPanTrigger, "pan_trigger", "Trigger", "Pan", 0, kTriggerNames,
        "What starts the pan cycle: the host's beat, a MIDI note, or a "
        "hit in the sidechain or the input, as the Trigger panel sets it up."),
   ENUM(kParamPanLoop, "pan_loop", "Loop", "Pan", 0, kOnOff,
        "With MIDI or Audio: On keeps cycling until the next trigger, Off runs "
        "the shape once and holds at its End marker."),
   PCT(kParamPanEnd, "pan_end", "End", "Pan", 1.0,
       "Where a shape run once stops and holds, until the next trigger. Drag "
       "the marker in the editor or turn this; Sync and Loop ignore it."),
   ENUM(kParamWidTrigger, "wid_trigger", "Trigger", "Width", 0, kTriggerNames,
        "What starts the width cycle: the host's beat, a MIDI note, or a "
        "hit in the sidechain or the input, as the Trigger panel sets it up."),
   ENUM(kParamWidLoop, "wid_loop", "Loop", "Width", 0, kOnOff,
        "With MIDI or Audio: On keeps cycling until the next trigger, Off runs "
        "the shape once and holds at its End marker."),
   PCT(kParamWidEnd, "wid_end", "End", "Width", 1.0,
       "Where a shape run once stops and holds, until the next trigger. Drag "
       "the marker in the editor or turn this; Sync and Loop ignore it."),
};
#undef LIN
#undef PCT
#undef BIPCT
#undef LOG
#undef STEP
#undef ENUM

} // namespace

double beatsForRate(int rate) {
   // A beat is a quarter note, so one cycle of each of these is this many beats.
   static const double kBeats[kNumRates] = {0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0};
   if (rate < 0 || rate >= kNumRates)
      return 1.0;
   return kBeats[rate];
}

const ParamDesc *paramTable() { return kParams; }

const ParamDesc *paramById(uint32_t id) {
   return id < kNumParams ? &kParams[id] : nullptr;
}

const ParamDesc *paramByKey(const char *key) {
   if (!key)
      return nullptr;
   for (uint32_t i = 0; i < kNumParams; ++i)
      if (std::strcmp(kParams[i].key, key) == 0)
         return &kParams[i];
   return nullptr;
}

} // namespace shapeau
