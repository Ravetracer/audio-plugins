#pragma once

#include <cstdint>

#include "plugincore/params.h"

namespace threeohthree {

// The parameter model -- ParamDesc, ParamKind, the range mapping and the text
// formatting -- comes from the PluginCore shared library. Pulling it in here
// rather than qualifying every use keeps the plugin's code readable. The
// directive is scoped to this namespace, so nothing escapes into the global
// one.
using namespace plugincore;

// Parameter identifiers. These are persisted in preset files and plugin state,
// so the numeric values must never change: append new parameters at the end
// and never reorder or reuse an id.
enum ParamId : uint32_t {
   // VCO. Two controls, which is what the machine has.
   kParamWaveform = 0,
   kParamTuning,

   // VCF. The four-stage transistor ladder and the envelope that sweeps it.
   kParamCutoff,
   kParamResonance,
   kParamEnvMod,
   kParamDecay,
   kParamTracking,

   // Accent. On the hardware this came from the sequencer; here it comes from
   // note velocity, so the threshold is a control of its own.
   kParamAccent,
   kParamAccentThreshold,
   kParamAccentDecay,

   // Slide. The hardware's slide time is fixed by C35 and its resistor
   // network; it is a control here because the note that starts a slide now
   // comes from the host rather than from the machine's own sequencer.
   kParamSlideTime,

   // Drive. The one stage that is not in the schematic.
   kParamDrive,
   kParamTone,

   // Output.
   kParamVolume,

   // Mods.
   //
   // Everything in this block was a hard-coded constant in the engine, and
   // every one of them was a number that could not be read off the schematic:
   // fitted to a printed waveform, or chosen so that the instrument behaved the
   // way the machine is described as behaving. Leaving them hidden would have
   // meant asking everyone to accept one particular guess.
   //
   // They are also, not by coincidence, close to the list of things people
   // actually soldered into their own machines. No two 303s agreed anyway --
   // matched pairs, a posistor and twenty years of drift saw to that -- so
   // "the" 303 was never one sound. This is where a player picks theirs.
   kParamEnvBias,
   kParamEnvDepth,
   kParamAccSweep,
   kParamAccBuild,
   kParamAccGain,
   kParamAccDecay,
   kParamDroop,
   kParamLadder,
   kParamResRange,
   kParamDrift,

   // Sequencer. The pattern itself is not here -- it is sixteen steps of
   // pitch, octave and three flags, which is not a thing a parameter table can
   // hold sensibly -- but everything about how it is played is.
   kParamMode,
   kParamSeqRate,
   kParamSeqSteps,
   kParamGate,
   kParamSwing,

   // Vibrato, per step. Another thing people added to their own machines.
   kParamVibDepth,
   kParamVibRate,
   kParamVibDelay,

   // The pattern generator. A seed and a set of densities, which between them
   // describe a pattern completely: the same seed and the same settings always
   // produce the same sixteen steps, so a pattern is a number you can write
   // down rather than something you have to save.
   kParamRandSeed,
   kParamRandScale,
   kParamRandRoot,
   kParamRandNotes,
   kParamRandAccent,
   kParamRandSlide,
   kParamRandOctave,
   kParamRandVibrato,

   kNumParams
};

// The waveform switch, S1 on the main board. Two positions, no blend: the
// hardware routes one or the other into the filter.
enum WaveformKind { kWaveSawtooth = 0, kWaveSquare, kNumWaveforms };

// Where the notes come from.
enum PlayMode { kModeMidi = 0, kModeSequencer, kNumPlayModes };

// The sequencer's clock, as a fraction of a beat per step.
enum RateKind { kRate32 = 0, kRate16T, kRate16, kRate8T, kRate8, kNumRates };

// Steps per beat for each of those.
double stepsPerBeat(int rate);

// Which notes the generator may use.
enum ScaleKind {
   kScaleMinor = 0,
   kScaleMajor,
   kScaleMinorPent,
   kScaleMajorPent,
   kScaleDorian,
   kScalePhrygian,
   kScaleBlues,
   kScaleChromatic,
   kNumScales
};

// Writes the scale's semitone offsets into `out` (at most 12) and returns how
// many there are. The root is always the first of them.
int scaleNotes(int scale, int *out);

// The plugin's own table, and lookups into it.
const ParamDesc *paramTable();
const ParamDesc *paramById(uint32_t id);
const ParamDesc *paramByKey(const char *key);

} // namespace threeohthree
