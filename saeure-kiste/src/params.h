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

   // The delay.
   //
   // The second stage on this instrument that the machine does not have, and
   // built the same way the first one was: out of the literature, with the
   // source named beside each part of it in dsp/delay.h. It sits after
   // everything else, which is where a box plugged into the back of the
   // machine would have sat.
   kParamDelayOn,
   kParamDelaySync,
   kParamDelayTime,
   kParamDelayDivision,
   kParamDelayFeedback,
   kParamDelayMix,
   kParamDelayMode,
   kParamDelayWidth,

   // Live mode's pattern transpose.
   //
   // In MIDI and Sequencer mode a held key moves the pattern by semitones,
   // which is what the machine's own keyboard did. Live mode gives the keys to
   // the pattern map instead, so the transpose has to be a control -- and an
   // octave is the one a bass line is actually moved by.
   kParamPatternOctave,

   // When a newly selected pattern takes over from the playing one. Appended
   // rather than beside the other bank controls because a parameter id is
   // persisted and may never move.
   kParamPatternTrigger,
   // How many times each pattern in a chain plays before the chain moves on.
   //
   // This and Chain Mode are legacy: both are per pattern now and live in the
   // bank. The ids stay because they are persisted -- an old preset, blob or
   // automation lane that sets one sets it for every pattern -- and the host is
   // told they are hidden. The window still draws them, reading and writing the
   // pattern on screen; see the plugin's GuiDelegate.
   kParamChainRepeat,

   kNumParams
};

// The waveform switch, S1 on the main board. Two positions, no blend: the
// hardware routes one or the other into the filter.
enum WaveformKind { kWaveSawtooth = 0, kWaveSquare, kNumWaveforms };

// Where the notes come from.
//
// Live is Sequencer with the keyboard doing something else. In Sequencer mode
// a held key transposes the pattern, which is the machine's own behaviour and
// is wrong for a set played off a pad controller: there the pads have to
// select patterns, and the pattern has to play at the pitch it was written at.
// So in Live mode a key that the pattern map knows about switches patterns, a
// key it does not know about simply runs the pattern as written, and the
// Pattern Oct parameter is what moves it.
enum PlayMode { kModeMidi = 0, kModeSequencer, kModeLive, kNumPlayModes };

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

// What happens when a pattern has played through. Each pattern carries its own
// -- see PatternData -- so a chain is a route through the bank rather than one
// rule for all of it. Next wraps round at Chain Length, Random picks from
// inside it, and Stay repeats the pattern for ever.
enum ChainKind { kChainStay = 0, kChainNext, kChainFirst, kChainRandom, kNumChainModes };

// The highest repeat count a pattern can carry.
constexpr int kMaxChainRepeat = 256;

// Where a chain is: which pattern plays on `cycle`, and how many cycles of it
// have started, that one included. Carried from one call to the next so a
// running chain only ever walks forward; see chainPatternAt().
struct ChainWalk {
   int start = -1; // the pattern the walk began from; -1 forces a fresh walk
   long cycle = 0;
   int pattern = 0;
   long plays = 1;
};

// Which pattern plays on the `cycle`-th time round, counting from `start`,
// given each pattern's chain mode and repeat count.
//
// Worked out by walking the chain from `start`, so it is still a function of
// the host's beat position: a loop or a seek backwards walks again from the
// top and lands on exactly the pattern it should. `walk` remembers where the
// last call got to, so ordinary playback advances one cycle at a time -- and
// an edit to a pattern's chain or repeat changes what comes next rather than
// rewriting the history that led to the pattern playing now.
int chainPatternAt(ChainWalk &walk, const int *modes, const int *repeats, int start,
                   int chainLength, long cycle);

// When a pattern selected while the sequencer runs takes over. At End lets the
// playing pattern reach its last step, which is the machine. Instant switches
// on the next step and carries on from the same place in the new pattern.
// Restart switches on the next step as well, but plays the new pattern from
// its first step.
enum TriggerKind { kTriggerAtEnd = 0, kTriggerInstant, kTriggerRestart, kNumTriggerModes };

// Whether the delay takes its time from the Time knob or from the host's
// tempo. Two positions rather than a flag, because it is drawn as a chip.
enum DelaySyncKind { kDelayFree = 0, kDelaySynced, kNumDelaySyncKinds };

// The note values the delay can lock to, from a thirty-second to a half note.
// A wider list than the sequencer's, because a delay is usually longer than a
// step: the dotted eighth in the middle of it is the one everybody reaches for.
enum DelayDivision {
   kDelayDiv32 = 0,
   kDelayDiv16T,
   kDelayDiv16,
   kDelayDiv8T,
   kDelayDiv8,
   kDelayDiv8Dot,
   kDelayDiv4T,
   kDelayDiv4,
   kDelayDiv2,
   kNumDelayDivisions
};

// How many beats one of those is. What the delay time is worked out from, at
// whatever tempo the host is reporting.
double delayDivisionBeats(int division);

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
