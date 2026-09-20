#pragma once

#include <cstdint>

#include "plugincore/params.h"

namespace saeurekiste {

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

   // The pattern bank. The steps themselves are still not parameters -- see
   // pattern.h -- but which of the sixty-four patterns is selected, what
   // happens when one has played through, and how many of them the chain
   // covers all are: a host that can automate a pattern change can arrange a
   // track with it, and all three travel in presets for free.
   kParamPattern,
   kParamChainMode,
   kParamChainLength,

   // The Devil Fish controls.
   //
   // Robin Whittle's modification of the machine, from the manual on his own
   // site. It is a different kind of source from the service notes -- Whittle
   // documents what each addition *does*, in numbers, rather than printing a
   // schematic -- but it is a source, which is what this plugin needs: none of
   // these is a guess.
   //
   // Every one of them defaults to the stock machine, and Whittle makes the
   // same point himself: a Devil Fish can still sound exactly like a TB-303,
   // and his manual has a table of where to leave each control to keep it
   // there. Those are the defaults below, so nothing that already existed
   // moves.
   //
   // Not affiliated with or endorsed by Robin Whittle. *Devil Fish* is his and
   // is used only to name what was modelled.
   kParamOverdrive,
   kParamFilterFM,
   kParamMuffler,
   kParamSoftAttack,
   kParamAmpDecay,
   kParamAmpSustain,
   kParamSweepSpeed,
   kParamAccentHold,

   // The drive stage's character and how much of it is heard.
   //
   // Drive already said how hard the stage is pushed; these two say what it is
   // being pushed into and how much of the result is mixed back with the
   // clean signal. Appended here rather than beside Drive because a parameter
   // id is persisted in presets and state and may never move.
   kParamDistType,
   kParamDistMix,
   kParamDistBias,

   kNumParams
};

// The waveform switch, S1 on the main board. Two positions, no blend: the
// hardware routes one or the other into the filter.
enum WaveformKind { kWaveSawtooth = 0, kWaveSquare, kNumWaveforms };

// Where the notes come from.
enum PlayMode { kModeMidi = 0, kModeSequencer, kNumPlayModes };

// The muted clipper on the VCA output. Two kinds of clipping rather than one,
// because the modification offers two.
enum MufflerKind { kMufflerOff = 0, kMufflerSoft, kMufflerHard, kNumMufflerKinds };

// The drive stage's models live in dsp/drive.h, as DriveModel, because each of
// them is an equation out of a named source rather than a name on a switch.
// The parameter table's enum is that list.

// How the accent sweep responds to accents in quick succession. Normal is the
// machine: charge left over from one accent makes the next one bigger. See
// acid_engine.cpp for what the other two do and where they come from.
enum SweepSpeed { kSweepNormal = 0, kSweepFast, kSweepSlow, kNumSweepSpeeds };

// What happens when a pattern has played through. The chain is patterns 1 to
// Chain Length; Stay ignores it and repeats the selected pattern for ever.
enum ChainKind { kChainStay = 0, kChainNext, kChainFirst, kChainRandom, kNumChainModes };

// Which pattern plays on the `cycle`-th time round, counting from the one the
// Pattern parameter selects. Derived from the cycle rather than counted up, so
// the sequencer stays a pure function of the host's beat position: a loop, a
// seek or a scrub lands on exactly the pattern it should.
int chainPatternAt(int mode, int start, int chainLength, long cycle);

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

} // namespace saeurekiste
