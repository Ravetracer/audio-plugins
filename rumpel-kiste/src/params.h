#pragma once

#include <cstdint>

#include "plugincore/params.h"

namespace rumpelkiste {

// The parameter model -- ParamDesc, ParamKind, the range mapping and the text
// formatting -- comes from the PluginCore shared library. The directive is
// scoped to this namespace, so nothing escapes into the global one.
using namespace plugincore;

// Parameter identifiers. These are persisted in preset files and plugin state,
// so the numeric values must never change: append new parameters at the end
// and never reorder or reuse an id. The table in params.cpp is indexed by id,
// so a new row goes at the bottom of it too.
enum ParamId : uint32_t {
   // The front panel, voice by voice, in the order the machine prints them
   // left to right: bass drum, snare, the three toms, rim shot and hand clap,
   // the hi-hat, the two cymbals.
   kParamBdTune = 0,
   kParamBdLevel,
   kParamBdAttack,
   kParamBdDecay,

   kParamSdTune,
   kParamSdLevel,
   kParamSdTone,
   kParamSdSnappy,

   kParamLtTune,
   kParamLtLevel,
   kParamLtDecay,
   kParamMtTune,
   kParamMtLevel,
   kParamMtDecay,
   kParamHtTune,
   kParamHtLevel,
   kParamHtDecay,

   kParamRsLevel,
   kParamCpLevel,

   kParamHhLevel,
   kParamChDecay,
   kParamOhDecay,

   kParamCrLevel,
   kParamCrTune,
   kParamRdLevel,
   kParamRdTune,

   // The two knobs at the right-hand end of the machine: Total Accent and the
   // master volume.
   kParamAccent,
   kParamVolume,

   // Which MIDI velocities count as an accent. The machine's own MIDI input
   // had to answer the same question.
   kParamAccentThreshold,

   // The sequencer. The steps themselves are not parameters -- see pattern.h --
   // but everything about how they are played is.
   kParamMode,
   kParamScale,
   kParamSteps,
   kParamShuffle,
   kParamFlam,

   // The bank: which pattern is selected, what happens when one has played
   // through, and when a newly selected one takes over. Chain Mode and Chain
   // Repeat are per pattern and live in the bank; the two parameters are the
   // handles the window draws them through, and are hidden from the host.
   // SäureKiste arrived at this arrangement over three releases, and this
   // plugin starts from where it ended up.
   kParamPattern,
   kParamChainMode,
   kParamChainLength,
   kParamPatternTrigger,
   kParamChainRepeat,

   // The pattern generator: a seed and three settings, which between them
   // describe a pattern completely.
   kParamGenSeed,
   kParamGenStyle,
   kParamGenBusy,
   kParamGenAccent,

   // Mods.
   //
   // The governing rule of this repository's machine models: everything that
   // can be read off the schematic is fixed, and everything that cannot is a
   // control rather than a hidden guess. The service notes give the time
   // constants, the filter corners, the resonator tunings and the ratios
   // between the oscillators in each voice; they do not give the absolute
   // frequency any oscillator runs at, how deep the pitch sweeps are, or what
   // is in the cymbal ROMs. Those are here.
   kParamBdPitch,
   kParamBdSweep,
   kParamBdShape,
   kParamSdPitch,
   kParamTomPitch,
   kParamTomSweep,
   kParamTomNoise,
   kParamRsDecay,
   kParamCpSpread,
   kParamHatColor,
   kParamCymColor,
   kParamDacBits,
   kParamLocalAccent,
   kParamShuffleUnit,
   kParamFlamUnit,
   kParamFlamGrace,

   // The drive bus: SäureKiste's drive stage, all ten models, put where a
   // desk's insert would go. Mode says what goes through it -- nothing, the
   // whole mix, or only the voices whose route switch is on -- and the eleven
   // switches after it are those routes.
   kParamDriveMode,
   kParamDistType,
   kParamDrive,
   kParamDistBias,
   kParamDriveTone,
   kParamDistMix,
   kParamRouteBd,
   kParamRouteSd,
   kParamRouteLt,
   kParamRouteMt,
   kParamRouteHt,
   kParamRouteRs,
   kParamRouteCp,
   kParamRouteCh,
   kParamRouteOh,
   kParamRouteCr,
   kParamRouteRd,

   kNumParams
};

// The eleven voices, in the order the machine numbers its keys. Also the
// sequencer's instrument rows, below the accent row; see pattern.h.
enum Voice {
   kVoiceBD = 0,
   kVoiceSD,
   kVoiceLT,
   kVoiceMT,
   kVoiceHT,
   kVoiceRS,
   kVoiceCP,
   kVoiceCH,
   kVoiceOH,
   kVoiceCR,
   kVoiceRD,
   kNumVoices
};

// What goes through the drive bus.
enum DriveMode { kDriveOff = 0, kDriveMaster, kDriveSelected, kNumDriveModes };

// Where the notes come from. Sequencer plays the bank locked to the host's
// transport; MIDI notes still play the voices in both modes, because a drum
// machine that ignored its pads while the pattern ran would be no use at all.
enum PlayMode { kModeMidi = 0, kModeSequencer, kNumPlayModes };

// The machine's SCALE button, in the order it steps through them. Owner's
// manual p.24: a step is a sixteenth, an eighth-note triplet, a thirty-second
// or a sixteenth-note triplet.
enum ScaleKind { kScale16 = 0, kScale8T, kScale32, kScale16T, kNumScales };

// Steps per beat for each of those.
double stepsPerBeat(int scale);

// What happens when a pattern has played through. Next wraps round at Chain
// Length, Random picks from inside it, and Stay repeats the pattern for ever.
enum ChainKind { kChainStay = 0, kChainNext, kChainFirst, kChainRandom, kNumChainModes };

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
// given each pattern's chain mode and repeat count. A function of the host's
// beat position -- a loop or a seek backwards walks again from the top -- with
// `walk` as a cache so ordinary playback advances one cycle at a time. The
// same function as SäureKiste's, for the same reasons.
int chainPatternAt(ChainWalk &walk, const int *modes, const int *repeats, int start,
                   int chainLength, long cycle);

// When a pattern selected while the sequencer runs takes over. At End lets the
// playing pattern reach its last step, which is the machine.
enum TriggerKind { kTriggerAtEnd = 0, kTriggerInstant, kTriggerRestart, kNumTriggerModes };

// What the generator writes. Each is a set of per-step likelihoods for every
// row; see generatePattern() in pattern.cpp.
enum GenStyle {
   kStyleHouse = 0,
   kStyleTechno,
   kStyleElectro,
   kStyleBreaks,
   kStyleGarage,
   kStyleWild,
   kNumGenStyles
};

// The plugin's own table, and lookups into it.
const ParamDesc *paramTable();
const ParamDesc *paramById(uint32_t id);
const ParamDesc *paramByKey(const char *key);

} // namespace rumpelkiste
