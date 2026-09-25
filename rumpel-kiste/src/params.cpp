#include "params.h"

#include "plugincore/param_macros.h"

#include <cmath>
#include <cstdint>

namespace rumpelkiste {

namespace {

const char *const kModeNames[] = {"MIDI", "Sequencer"};
const char *const kScaleNames[] = {"1/16", "1/8T", "1/32", "1/16T"};
const char *const kChainNames[] = {"Stay", "Next", "First", "Random"};
const char *const kTriggerNames[] = {"At End", "Instant", "Restart"};
// A preset stores the *name*, so this list may be appended to but never
// reordered.
const char *const kStyleNames[] = {"House", "Techno", "Electro", "Breaks", "Garage", "Wild"};
const char *const kDriveModeNames[] = {"Off", "Master", "Selected"};
// SäureKiste's models, in its order. A preset stores the name, so this list
// may be appended to but never reordered.
const char *const kDistNames[] = {"Soft Clip", "Overdrive", "Tube",      "Valve Stack",
                                  "Fuzz",      "Rectifier", "Crush",     "Germanium",
                                  "Crunch",    "Lead"};
const char *const kRouteNames[] = {"Dry", "Drive"};

// Where the numbers come from.
//
// Every time constant, filter corner, resonator tuning and oscillator ratio
// below is computed from component values on the voicing board's circuit
// diagram (TR-909 service notes, June 15 1984, page 11) or taken from the
// circuit descriptions on pages 5 and 6. tools/analysis/README.md lists each
// one with the parts it was read from. The tips say so where it matters.
//
// The mods at the bottom are the numbers the service notes do not give.
const ParamDesc kParams[kNumParams] = {
   // ------------------------------------------------------------ bass drum
   PCT(kParamBdTune, "bd_tune", "Tune", "Bass Drum", 0.5,
       "VR2, and it does not do what its name says. It sits in the discharge path of "
       "C9 (0.33 uF, with R57 22 k in series), which is the envelope that sweeps the "
       "oscillator down after the hit -- so it sets how long the pitch stays up, from a "
       "7.6 ms time constant to 29 ms on a machine, not the note the kick settles on. That note is "
       "fixed by R27's bias; it is BD Pitch under ADVANCED. The owner's manual says as "
       "much: whatever the Tune setting, the Decay knob works at its own time."),
   PCT(kParamBdLevel, "bd_level", "Level", "Bass Drum", 0.8,
       "VR4, 100 k linear, in the feedback of the bass drum's output amplifier."),
   PCT(kParamBdAttack, "bd_attack", "Attack", "Bass Drum", 0.5,
       "VR3, a 500 ohm pot mixing two things into the click at the front of the kick: "
       "a pulse from Q8/Q9 and the shared noise, low-passed by R45 and C13. Both go "
       "through Q6, whose envelope is C12 (0.033 uF) discharging through R41 (22 k): "
       "0.73 ms. Even fully down a machine still clicks a little, and the knob does most "
       "of its work in the top half of its travel."),
   LOG(kParamBdDecay, "bd_decay", "Decay", "Bass Drum", 0.5, 15.5, 345.0, "ms",
       "VR5, 1 M audio taper, with R58 (47 k) in series, discharging C8 (0.33 uF): the "
       "amplitude envelope's time constant runs from 15.5 ms to 345 ms, which is what "
       "this knob shows. What a machine does with it: the kick holds its level for "
       "45 ms whatever this says, then falls in two stages over 16 ms to 64 ms. The "
       "owner's manual's advice for muting the kick is to turn this fully down."),

   // ---------------------------------------------------------------- snare
   PCT(kParamSdTune, "sd_tune", "Tune", "Snare Drum", 0.5,
       "VR6, 10 k linear, between R287 (100 k) and R284 (10 k). The voltage it taps "
       "runs over a range of exactly two to one, and the oscillators' frequency follows "
       "it, so the knob spans one octave. Both of the snare's oscillators move together; "
       "their ratio is fixed by their capacitors."),
   PCT(kParamSdLevel, "sd_level", "Level", "Snare Drum", 0.8,
       "VR8, 50 k linear, in the feedback of IC40b, which sums both oscillators and "
       "the noise."),
   PCT(kParamSdTone, "sd_tone", "Tone", "Snare Drum", 0.5,
       "VR7, 500 k linear, which the owner's manual calls timbre and the circuit "
       "calls a decay: it discharges C67 (0.47 uF) together with R254 (100 k), and that "
       "envelope opens Q48 onto the low-passed noise. The parts give the tail a time "
       "constant of 47 ms to 282 ms; a machine gives 24 ms to 77 ms, and that is what "
       "this does. A longer tail is the brighter snare."),
   PCT(kParamSdSnappy, "sd_snappy", "Snappy", "Snare Drum", 0.5,
       "VR9, 10 k linear, the gain of IC39b, which mixes the two noise paths: a "
       "short burst of high-passed noise (IC39a, 4.0 kHz) as long as the trigger "
       "pulse, and the longer low-passed tail (IC40a, 8.5 kHz) that Tone sets. Fully "
       "down is the drum with the snares off."),

   // ----------------------------------------------------------------- toms
   PCT(kParamLtTune, "lt_tune", "Tune", "Low Tom", 0.5,
       "VR10, 10 k linear between R113 (68 k) and R112 (10 k). The same two-to-one "
       "divider as the snare's, so each tom tunes over one octave. The owner's manual "
       "suggests the three of them as a melody tom, and an octave each is what lets "
       "them overlap."),
   PCT(kParamLtLevel, "lt_level", "Level", "Low Tom", 0.8, "VR12, 50 k linear."),
   LOG(kParamLtDecay, "lt_decay", "Decay", "Low Tom", 0.598, 38.0, 378.0, "ms",
       "VR11, 500 k linear with R111 (56 k), discharging C23 (0.68 uF): 38 ms to 378 "
       "ms, which is what the knob shows. What reaches the ear is shorter and moves "
       "less, as the machine does: the tom holds for a moment and then dies away "
       "with a time constant of 46 to 114 ms."),
   PCT(kParamMtTune, "mt_tune", "Tune", "Mid Tom", 0.5,
       "VR13. One octave, like the low tom's. The mid tom's timing capacitors are "
       "0.018, 0.027 and 0.01 uF against the low tom's 0.022, 0.033 and 0.012, so at "
       "the same setting it sits 1.22 times higher."),
   PCT(kParamMtLevel, "mt_level", "Level", "Mid Tom", 0.8, "The mid tom's level, 50 k linear like the low tom's."),
   LOG(kParamMtDecay, "mt_decay", "Decay", "Mid Tom", 0.598, 38.0, 378.0, "ms",
       "The same envelope as the low tom's, from the same part values. The mid tom "
       "still dies away faster than the low one, at 32 to 79 ms."),
   PCT(kParamHtTune, "ht_tune", "Tune", "Hi Tom", 0.5,
       "VR16. One octave. The hi tom's capacitors are 0.015, 0.022 and 0.0082 uF, so "
       "at the same setting it sits 1.5 times above the low tom."),
   PCT(kParamHtLevel, "ht_level", "Level", "Hi Tom", 0.8, "The hi tom's level, 50 k linear like the low tom's."),
   LOG(kParamHtDecay, "ht_decay", "Decay", "Hi Tom", 0.598, 38.0, 378.0, "ms",
       "The same envelope as the low tom's, from the same part values. The hi tom "
       "dies away at 34 to 84 ms."),

   // ------------------------------------------------------ rim shot, clap
   PCT(kParamRsLevel, "rs_level", "Rim Shot", "Rim / Clap", 0.7,
       "VR19, the rim shot's level. The voice itself has no other control: three "
       "bridged-T resonators rung by one pulse, near 220 Hz, 490 Hz and 1 kHz, "
       "clipped hard by D91/D92 for the first few milliseconds and then left to ring "
       "on at the lowest of them."),
   PCT(kParamCpLevel, "cp_level", "Hand Clap", "Rim / Clap", 0.7,
       "VR20, the hand clap's level. Noise through a 960 Hz band-pass, cut into four "
       "bursts by a sawtooth envelope, with a darker tail that comes in with the last "
       "one."),

   // --------------------------------------------------------------- hi-hat
   PCT(kParamHhLevel, "hh_level", "Level", "Hi-Hat", 0.7,
       "VR22. One level for both hats, because on the machine they are one voice: the "
       "same ROM, read from two different places, through one amplifier. A closed hat "
       "cuts off an open one for the same reason."),
   LOG(kParamChDecay, "ch_decay", "CH Decay", "Hi-Hat", 0.5, 10.0, 110.0, "ms",
       "VR21. The hats' amplifier is an anti-log converter driven by C135 charging, so "
       "the level falls linearly in decibels. The closed hat charges it through R451 "
       "(10 k) and VR21 (100 k), which is what this shows; C135's value is not legible, "
       "and a machine's closed hat dies away with a time constant of 9 ms to 45 ms "
       "over the knob's travel."),
   LOG(kParamOhDecay, "oh_decay", "OH Decay", "Hi-Hat", 0.5, 100.0, 1100.0, "ms",
       "VR23, 1 M, with R452 (100 k): ten times the closed hat's charging path, which "
       "is the ratio the service notes state. A machine's open hat is not ten times "
       "longer: 120 ms at the top of the knob against the closed hat's 45 ms."),

   // -------------------------------------------------------------- cymbals
   PCT(kParamCrLevel, "cr_level", "Level", "Crash", 0.6, "VR24, the crash's level."),
   PCT(kParamCrTune, "cr_tune", "Tune", "Crash", 0.5,
       "The clock that reads the crash ROM, from the 4011 oscillator VR25 tunes. A "
       "faster clock plays the whole cymbal higher and shorter, which is what "
       "retuning a sample does and what the owner's manual means by pitch. The range "
       "is not printed; a machine's runs from 28.5 kHz to 45.8 kHz, 37 kHz at the "
       "centre, about four semitones either way."),
   PCT(kParamRdLevel, "rd_level", "Level", "Ride", 0.6, "VR26, the ride's level."),
   PCT(kParamRdTune, "rd_tune", "Tune", "Ride", 0.5,
       "VR27, the ride ROM's clock, like the crash's."),

   // --------------------------------------------------------------- master
   PCT(kParamAccent, "accent", "Accent", "Master", 0.7,
       "Total Accent. On the machine an accented step raises every voice sounding on "
       "it, and this sets by how much. Each voice takes accent differently -- a louder "
       "kick with a harder click and a higher sweep, a snare with more snap -- because "
       "the accent voltage reaches every envelope in it, not just the level."),
   LIN(kParamVolume, "volume", "Volume", "Master", -60.0, 0.0, 0.0, "dB",
       "The master level. Stops at unity; the machine had no gain above it either. An "
       "unaccented hit on the machine is about a third as loud as an accented one, so a "
       "pattern with no accents in it sits low -- the accent row is the first thing to reach "
       "for, before this."),
   STEP(kParamAccentThreshold, "accent_threshold", "Accent At", "Master", 1.0, 127.0, 100.0, "",
        "Which MIDI velocities count as an accent. The machine's accent is one bit per "
        "voice per step, a switch rather than a curve, so a note at or above this "
        "velocity is accented and one below is not."),

   // ------------------------------------------------------------ sequencer
   ENUM(kParamMode, "mode", "Mode", "Sequencer", 1.0, kModeNames,
        "Whether the pattern plays. Sequencer runs the bank locked to the host's "
        "transport, or on its own with the PLAY button when the host is stopped. MIDI "
        "switches the pattern off and leaves the host to play the voices. Incoming notes "
        "play the voices in both, on the machine's own key numbers: 36 bass drum, 38 "
        "snare, 42 closed hat, 46 open hat and so on."),
   ENUM(kParamScale, "scale", "Scale", "Sequencer", 0.0, kScaleNames,
        "What one step is worth: a sixteenth, an eighth-note triplet, a thirty-second "
        "or a sixteenth-note triplet, in the order the machine's SCALE button steps "
        "through them. The machine set the last step to 12 for the triplet scales; "
        "here Last Step is left where you put it."),
   STEP(kParamSteps, "steps", "Last Step", "Sequencer", 1.0, 64.0, 16.0, "",
        "How many steps the pattern runs before it starts again. The machine stopped at "
        "sixteen; this goes to sixty-four, which is four bars of sixteenths, and a "
        "pattern longer than sixteen is scrolled to in the grid."),
   STEP(kParamShuffle, "shuffle", "Shuffle", "Sequencer", 1.0, 7.0, 1.0, "",
        "The machine's seven shuffle settings. 1 is straight, and every setting above it "
        "delays the second step of each pair by one more Shuffle Unit -- a twelfth of a "
        "step unless that mod says otherwise, so 7 is a triplet feel."),
   STEP(kParamFlam, "flam", "Flam", "Sequencer", 1.0, 8.0, 3.0, "",
        "The machine's eight flam intervals. A flammed step plays twice: a lighter "
        "stroke on the step and the full one Flam times Flam Unit later. The interval "
        "does not follow the tempo, which the owner's manual points out as well."),

   // ------------------------------------------------------------- the bank
   STEP(kParamPattern, "pattern", "Pattern", "Sequencer", 1.0, 64.0, 1.0, "",
        "Which of the sixty-four patterns the grid edits, and the one the chain starts "
        "from."),
   ENUM(kParamChainMode, "chain_mode", "Chain", "Sequencer", 0.0, kChainNames,
        "What happens when the pattern on screen has played through. Every pattern has "
        "its own. Stay repeats it. Next steps to the following pattern and wraps round "
        "at Chain Length. First goes back to pattern 1. Random picks one from inside "
        "the chain. Worked out from the host's beat position, so looping and scrubbing "
        "land on the pattern they should."),
   STEP(kParamChainLength, "chain_length", "Chain Length", "Sequencer", 1.0, 64.0, 4.0, "",
        "How many patterns the chain covers, counting from pattern 1: where Next wraps "
        "round and what Random picks from. One setting for the whole bank."),
   ENUM(kParamPatternTrigger, "pattern_trigger", "Trigger", "Sequencer", 0.0, kTriggerNames,
        "When a pattern selected while the sequencer runs takes over. At End lets the "
        "playing one reach its last step first, which is the machine. Instant switches "
        "on the next step and keeps the place in the bar. Restart switches on the next "
        "step and plays the new pattern from its first."),
   STEP(kParamChainRepeat, "chain_repeat", "Chain Repeat", "Sequencer", 1.0, 256.0, 1.0, "",
        "How many times the pattern on screen plays before its chain moves on. Every "
        "pattern has its own. Stay ignores it."),

   // ------------------------------------------------------------ generator
   STEP(kParamGenSeed, "gen_seed", "Seed", "Generator", 0.0, 4294967295.0, 1.0, "",
        "The generator's seed. The same seed with the same Style, Busy and Accents "
        "always writes the same pattern, so a groove you like is a number you can write "
        "down. GEN picks a new one; - and + step through them."),
   ENUM(kParamGenStyle, "gen_style", "Style", "Generator", 0.0, kStyleNames,
        "What the generator knows about. Each style is a set of likelihoods for every "
        "voice on every step of the bar -- where the kick lands, where the claps go, "
        "whether the hats run in sixteenths or sit on the off-beat. Wild throws most of "
        "that away."),
   PCT(kParamGenBusy, "gen_busy", "Busy", "Generator", 0.5,
       "How much the generator writes. At the bottom it keeps only what defines the "
       "style; towards the top it fills in ghost notes, tom runs and extra hats. Every "
       "step draws all of its random numbers whatever this is set to, so turning it "
       "up adds hits to the pattern you had rather than writing a different one."),
   PCT(kParamGenAccent, "gen_accent", "Accents", "Generator", 0.35,
       "How many of the generated steps carry an accent, on the accent row and on the "
       "voices. Changing it moves accents and nothing else."),

   // ----------------------------------------------------------------- mods
   LOG(kParamBdPitch, "bd_pitch", "BD Pitch", "Mods", 0.3777, 35.0, 90.0, "Hz",
       "The frequency the kick settles on. It is set by R27 (1.5 M) feeding the "
       "oscillator's integrator, and the service notes give the resistor but not the "
       "frequency it produces, so this is a mod rather than a guess baked in."),
   LIN(kParamBdSweep, "bd_sweep", "BD Sweep", "Mods", 1.0, 8.0, 5.6, "x",
       "How far above BD Pitch the kick starts, as a ratio. C9's envelope sweeps it, "
       "for as long as Tune says; how high it drives the oscillator depends on levels "
       "the schematic does not print. The default is the machine's: a kick that "
       "settles near 50 Hz starts near 280, at every accent."),
   PCT(kParamBdShape, "bd_shape", "BD Shape", "Mods", 0.5,
       "How hard the triangle is driven into D10/D11, the diode pair that rounds it "
       "towards a sine. Low leaves more of the triangle's edge; high flattens the tops "
       "into something closer to a square. The triangle's amplitude against the "
       "diodes' knee is not printed."),
   LOG(kParamSdPitch, "sd_pitch", "SD Pitch", "Mods", 0.4055, 120.0, 300.0, "Hz",
       "The snare's lower oscillator at Tune's centre. The upper one runs 1.5 times "
       "higher -- their capacitors (C69 0.01 uF, C71 0.0068 uF) say 1.47, a machine "
       "measures 1.50 -- and that is fixed. Both bend up at the hit by the 5 V to 2 V "
       "swing of IC36's supply the service notes draw, which is fixed too."),
   LOG(kParamTomPitch, "tom_pitch", "Tom Pitch", "Mods", 0.4663, 50.0, 160.0, "Hz",
       "The low tom's pitch at Tune's centre once the sweep has settled: its middle "
       "oscillator, the one the ear follows. Everything else about the toms' pitch "
       "follows from capacitors: each tom's lowest oscillator runs a fifth under it "
       "and its top one 1.83 times over it, the mid tom sits 1.22 and the hi tom 1.5 "
       "times above the low one."),
   PCT(kParamTomSweep, "tom_sweep", "Tom Sweep", "Mods", 0.5,
       "How far the toms bend down at the hit. Two envelopes add their current to "
       "Tune's, one gone in about 30 ms and C16's (0.1 uF, 2.2 M: 220 ms), so the "
       "bend is a number of hertz rather than an interval: a tom tuned low bends "
       "further, in pitch, than one tuned high. At 50 % a tom at Tune's centre starts "
       "38 % sharp."),
   PCT(kParamTomNoise, "tom_noise", "Tom Noise", "Mods", 0.5,
       "The noise at the front of every tom: the shared noise high-passed at 720 Hz, "
       "a short wash under the hit and a tick from C54 and R198 as the trigger ends. "
       "Its level against the oscillators is not printed; 50 % is the machine's."),
   LOG(kParamRsDecay, "rs_decay", "Rim Gate", "Mods", 0.6856, 2.0, 200.0, "ms",
       "How long Q65 lets the rim shot through: C119 (0.047 uF) discharging into "
       "R403 (1 M), 47 ms. The resonators ring out well inside that, so the default "
       "leaves the hit as long as they make it; turned down, the gate cuts the "
       "low ring off and leaves the click."),
   LOG(kParamCpSpread, "cp_spread", "Clap Spread", "Mods", 0.6055, 4.0, 20.0, "ms",
       "The gap between the clap's four bursts. The service notes' printed waveform "
       "shows four; a machine spaces them 10.0, 11.5 and 10.3 ms apart, 10.6 ms on "
       "average, which is the default, and this keeps that uneven pattern. The "
       "oscillator that times them is not legible."),
   PCT(kParamHatColor, "hat_color", "Hat Color", "Mods", 0.5,
       "The hats on the machine are samples of real ones, stored as six-bit PCM in a "
       "ROM that is not in the service notes and is not ours to have. What plays "
       "instead is a synthesised metal source put through the same six-bit converter, "
       "the same 31.5 kHz clock and the same decay circuit, shaped to a machine's "
       "spectrum at 50 %. This sets how bright that source is."),
   PCT(kParamCymColor, "cym_color", "Cym Color", "Mods", 0.5,
       "The same for the crash and the ride: how bright the synthesised source is that "
       "stands in for their ROMs. At 50 % it is shaped to a machine's spectrum, the "
       "ride's bell partials where the recording has them."),
   STEP(kParamDacBits, "dac_bits", "DAC Bits", "Mods", 4.0, 16.0, 6.0, "bit",
        "The resolution of the hats' and cymbals' converters. Six is the machine: the "
        "ROM data is six bits wide, latched into IC68 and converted by a resistor "
        "array. Because the samples were stored compressed and the decay is put back "
        "afterwards, the quantisation grit fades with the sound instead of sitting "
        "under it -- which is reproduced here and is much of why they sound as they do."),
   PCT(kParamLocalAccent, "local_accent", "Local Accent", "Mods", 0.6,
       "How strong an accent written on one voice is, against a Total Accent at full. "
       "The machine's per-voice accent is fixed and cannot be changed, as the owner's "
       "manual says -- but what it is fixed at comes from a resistor array whose values "
       "are not printed."),
   LIN(kParamShuffleUnit, "shuffle_unit", "Shuffle Unit", "Mods", 2.0, 16.0, 8.333, "%",
       "How much later each Shuffle setting above 1 puts the second step of a pair, as "
       "a share of a step. A twelfth is the default, which makes setting 7 the triplet "
       "feel; the machine's own step size is not given."),
   LIN(kParamFlamUnit, "flam_unit", "Flam Unit", "Mods", 1.0, 10.0, 4.0, "ms",
       "The flam interval per Flam setting. The owner's manual says the interval is "
       "fixed rather than tied to the tempo and gives eight of them, but not their "
       "length."),
   PCT(kParamFlamGrace, "flam_grace", "Flam Grace", "Mods", 0.6,
       "How loud the first, lighter stroke of a flam is against the second."),

   // ---------------------------------------------------------- drive bus
   ENUM(kParamDriveMode, "drive_mode", "Drive Mode", "Drive", 0.0, kDriveModeNames,
        "What goes through the drive. Off leaves the machine as it is. Master puts the "
        "whole mix through it, the way a drum machine plugged into a pedal sounds. "
        "Selected sends only the voices whose route switch says Drive and leaves the rest "
        "clean -- a distorted kick under crisp hats, or a clap into a fuzz on its own."),
   ENUM(kParamDistType, "dist_type", "Type", "Drive", 0.0, kDistNames,
        "The model, SäureKiste's ten: each an equation out of the literature or a "
        "pedal's own circuit, and built differently from the others rather than merely "
        "curved differently. Soft Clip is the gentlest. Overdrive keeps a clean region. "
        "Tube sits off centre and adds even harmonics. Valve Stack is three stages in a "
        "row. Fuzz has no clean region at all. Rectifier folds the wave and doubles the "
        "pitch, which on a kick is an octave up. Crush is bit reduction. Germanium is "
        "the MXR Distortion+, and Crunch and Lead are the two channels of the BOSS "
        "SD-2, both from their schematics."),
   PCT(kParamDrive, "drive", "Drive", "Drive", 0.35,
       "How hard the bus is pushed into the model, level-matched so that turning it up "
       "thickens rather than simply getting louder."),
   BIPCT(kParamDistBias, "dist_bias", "Bias", "Drive", 0.0,
         "Where the model sits on its own curve. Away from the centre the two halves of "
         "the wave are treated differently, which is what puts even harmonics in."),
   LOG(kParamDriveTone, "drive_tone", "Tone", "Drive", 0.739536, 800.0, 18000.0, "Hz",
       "A gentle low-pass after the model, for taking the top off what it adds. At the "
       "top of its range it is doing nothing."),
   PCT(kParamDistMix, "dist_mix", "Mix", "Drive", 1.0,
       "How much of the driven bus is heard against its clean self. Below 100 % a hard "
       "model becomes a layer under the drums rather than instead of them."),
   ENUM(kParamRouteBd, "route_bd", "BD", "Drive Route", 0.0, kRouteNames,
        "Whether this voice goes through the drive bus when Drive Mode is Selected. Ignored in the other two modes."),
   ENUM(kParamRouteSd, "route_sd", "SD", "Drive Route", 0.0, kRouteNames,
        "Whether this voice goes through the drive bus when Drive Mode is Selected. Ignored in the other two modes."),
   ENUM(kParamRouteLt, "route_lt", "LT", "Drive Route", 0.0, kRouteNames,
        "Whether this voice goes through the drive bus when Drive Mode is Selected. Ignored in the other two modes."),
   ENUM(kParamRouteMt, "route_mt", "MT", "Drive Route", 0.0, kRouteNames,
        "Whether this voice goes through the drive bus when Drive Mode is Selected. Ignored in the other two modes."),
   ENUM(kParamRouteHt, "route_ht", "HT", "Drive Route", 0.0, kRouteNames,
        "Whether this voice goes through the drive bus when Drive Mode is Selected. Ignored in the other two modes."),
   ENUM(kParamRouteRs, "route_rs", "RS", "Drive Route", 0.0, kRouteNames,
        "Whether this voice goes through the drive bus when Drive Mode is Selected. Ignored in the other two modes."),
   ENUM(kParamRouteCp, "route_cp", "CP", "Drive Route", 0.0, kRouteNames,
        "Whether this voice goes through the drive bus when Drive Mode is Selected. Ignored in the other two modes."),
   ENUM(kParamRouteCh, "route_ch", "CH", "Drive Route", 0.0, kRouteNames,
        "Whether this voice goes through the drive bus when Drive Mode is Selected. Ignored in the other two modes."),
   ENUM(kParamRouteOh, "route_oh", "OH", "Drive Route", 0.0, kRouteNames,
        "Whether this voice goes through the drive bus when Drive Mode is Selected. Ignored in the other two modes."),
   ENUM(kParamRouteCr, "route_cr", "CR", "Drive Route", 0.0, kRouteNames,
        "Whether this voice goes through the drive bus when Drive Mode is Selected. Ignored in the other two modes."),
   ENUM(kParamRouteRd, "route_rd", "RD", "Drive Route", 0.0, kRouteNames,
        "Whether this voice goes through the drive bus when Drive Mode is Selected. Ignored in the other two modes."),
};

#undef LIN
#undef PCT
#undef BIPCT
#undef LOG
#undef STEP
#undef ENUM

// Where a pattern in `mode` hands over to on the `cycle`-th time round.
int chainNext(int mode, int from, int span, long cycle) {
   switch (mode) {
   case kChainFirst:
      return 0;
   case kChainRandom: {
      // A hash of the cycle rather than a running generator: the same bar of
      // the song always picks the same pattern, however it was reached.
      uint32_t x = static_cast<uint32_t>(cycle) * 2654435761u + 0x9E3779B9u;
      x ^= x >> 16;
      x *= 0x7FEB352Du;
      x ^= x >> 15;
      return static_cast<int>(x % static_cast<uint32_t>(span));
   }
   case kChainNext:
   default:
      // A pattern outside the chain feeds into its first pattern.
      return from + 1 < span ? from + 1 : 0;
   }
}

} // namespace

double stepsPerBeat(int scale) {
   switch (scale) {
   case kScale8T:
      return 3.0;
   case kScale32:
      return 8.0;
   case kScale16T:
      return 6.0;
   case kScale16:
   default:
      return 4.0;
   }
}

int chainPatternAt(ChainWalk &walk, const int *modes, const int *repeats, int start,
                   int chainLength, long cycle) {
   const int span = chainLength < 1 ? 1 : (chainLength > 64 ? 64 : chainLength);
   const int from = start < 0 ? 0 : (start > 63 ? 63 : start);
   if (walk.start != from || cycle < walk.cycle)
      walk = ChainWalk{from, 0, from, 1};
   while (walk.cycle < cycle) {
      const int mode = modes[walk.pattern];
      if (mode == kChainStay) {
         // Nothing leaves a Stay, so there is nothing to walk through.
         walk.cycle = cycle;
         break;
      }
      ++walk.cycle;
      const int rep = repeats[walk.pattern] < 1 ? 1 : repeats[walk.pattern];
      if (walk.plays >= rep) {
         walk.pattern = chainNext(mode, walk.pattern, span, walk.cycle);
         walk.plays = 1;
      } else {
         ++walk.plays;
      }
   }
   return cycle <= 0 ? from : walk.pattern;
}

const ParamDesc *paramTable() { return kParams; }

const ParamDesc *paramById(uint32_t id) { return paramByIdIn(kParams, kNumParams, id); }

const ParamDesc *paramByKey(const char *key) { return paramByKeyIn(kParams, kNumParams, key); }

} // namespace rumpelkiste
