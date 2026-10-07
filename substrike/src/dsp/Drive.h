#pragma once

// The drive stage, ported from SaeureKiste 0.15.2 (saeure-kiste/src/dsp/drive.*)
// for Substrike's Distortion slot. The models, their gain staging and their
// level matching are unchanged; what changed is that the stage no longer
// oversamples itself. The notes below were written for SaeureKiste, so "this
// instrument" and "the amplifier" mean the TB-303 model there, and the
// loudness trims were measured on a bass line rather than a kick.
//
// Original note:
//
// The drive stage: the one part of this instrument that is not the TB-303.
//
// Everything in front of it is fixed by the February 1982 service notes. There
// is no schematic for this stage, so it is built the other way round -- from
// the literature. Every model below is an equation out of a published source,
// named at the point it is used:
//
//   [DAFX]    U. Zoelzer (ed.), "DAFX: Digital Audio Effects", 2nd edition,
//             Wiley 2011, chapter 4 "Nonlinear processing" (Dutilleux,
//             Dempwolf, Holters, Zoelzer). Equations 4.13 to 4.15 and the
//             M-files beside them.
//   [ESmash]  ElectroSmash, "MXR Distortion+ Analysis", at
//             electrosmash.mas-effects.com/mxr-distortion-plus-analysis.html --
//             a component-by-component reading of the pedal with the corner
//             frequencies worked out. Used for the Germanium model alongside
//             the schematic itself, and it agrees with the drawing everywhere
//             the two overlap.
//   [Pirkle]  W. Pirkle, "Designing Audio Effect Plugins in C++", 2nd edition,
//             Routledge 2019, chapter 19 "Nonlinear Processing: Distortion,
//             Tube Simulation, and HF Exciters". Tables 19.1 and 19.2,
//             equations 19.1 and 19.2, and the TriodeClassA / ClassATubePre
//             objects in sections 19.12 and 19.13.
//
// The books themselves are not in this repository and must not be: they are
// copyrighted and are not ours to redistribute. What is here is the equations,
// implemented, with the citation beside each one.
//
// Why the models are structured differently from each other, and not merely
// curved differently: because a set of memoryless odd-symmetric clippers all
// sound the same. Every one of them flattens into the same square wave once it
// is driven hard, and level-matching the outputs removes what little is left.
// That was the first attempt at this stage and it was a fair criticism of it.
// What makes these tell each other apart is structure -- a linear region that
// survives (Overdrive), an operating point off centre (Tube), three gain
// stages in series with a filter between them (Valve Stack), a rectifier that
// doubles the fundamental (Rectifier), a quantiser (Crush) -- and their own
// filtering, which no waveshaper alone can give.
//
// Both sources say the same thing about aliasing: a nonlinearity needs
// oversampling ([DAFX] section 4.1.1 and figure 4.5; [Pirkle] section 19.1).
// In Substrike the effect slot does that, at the rate the Quality setting
// picks, and prepare() is given the rate the stage actually runs at.

#include <cstdint>

#include "dsp/OnePole.h"

namespace substrike::dsp {

// The models, in the order the parameter lists them. A preset stores the
// *name*, so these may be reordered only at the cost of every preset that
// names one.
enum DriveModel {
   kDriveSoftClip = 0, // the stage this plugin always had
   kDriveOverdrive,    // [DAFX] eq 4.14
   kDriveTube,         // [DAFX] eq 4.13 + M-file 4.4
   kDriveValveStack,   // [Pirkle] 19.12/19.13
   kDriveFuzz,         // [DAFX] eq 4.15 + [Pirkle] FEXP1
   kDriveRectifier,    // [DAFX] 4.3.3, [Pirkle] table 19.2 HWR/FWR
   kDriveCrush,        // [Pirkle] eq 19.1
   kDriveGermanium,    // the MXR Distortion+ circuit, from its schematic
   kDriveCrunch,       // the BOSS SD-2's CRUNCH channel, from its service notes
   kDriveLead,         // and its LEAD channel, which is a different circuit
   kNumDriveModels
};

// The eighth is a different kind of source from the other seven, and that is
// the point of it. Six of them are equations out of a book and Soft Clip is
// this plugin's own; Germanium is a *circuit* -- the MXR Distortion+, whose
// schematic is a published document like the service notes the rest of this
// instrument is built from. So it is modelled the way the 303 is modelled:
// component by component, with every value read off the drawing, rather than
// by picking a curve that sounds a bit like it.
//
// What it is, in the order the signal meets it (component names are the ones
// on the schematic):
//
//   C2/R1/R2   a 16 Hz input highpass; a DC block and nothing more
//   U1         a 741 wired non-inverting, with C3 (47 n), R3 (4.7 k) and the
//              1 M DISTORTION pot in its lower leg. That capacitor is the
//              whole character of the pedal: the stage has *unity* gain at DC
//              and its full gain only above a corner that the pot moves. At
//              maximum the gain is 1 + 1M/4.7k = 214 and the corner is 720 Hz,
//              so the harmonics are lifted 46 dB and the fundamental is not.
//              Turning it down lowers the gain and drops the corner with it.
//              This is why a Distortion+ gets thinner as it gets dirtier, and
//              it is why the model does not flub on a bass line.
//   U1 again   the 741's gain-bandwidth product is 1 MHz, so at full gain the
//              stage cannot do better than 4.7 kHz. The part number is on the
//              schematic, so this is a number the circuit gives rather than a
//              taste decision, and it is most of why the pedal is not fizzy.
//   rails      9 V supply, biased at half of it: the output stops at about
//              +/-3.5 V, and at these gains it is there most of the time.
//   R5/D1/D2   the clipper, and it is a *shunt*: 10 k in series into two
//              anti-parallel diodes to ground. That series resistance is what
//              makes it soft -- the diodes do not clamp the signal, they load
//              it, and how much depends on how hard it is driven. A clipper
//              wired across the path like this cannot be written as a transfer
//              curve with a threshold, which is why the model solves the node
//              equation instead.
//   D1/D2      germanium 1N270, which the schematic names outright. Germanium
//              conducts from about 0.3 V and has a far softer knee than the
//              silicon every later revision of this pedal used.
//   C5         1 n across the node: a top-end roll-off at 15.9 kHz.
//
// Three things [ESmash] says are worth writing down, because they are checks
// on the model rather than inputs to it.
//
// It measures "a mid hump around 1.5 kHz", and that hump is not a component.
// It is what the shelf rising from 720 Hz and the 741 running out of gain at
// 4.7 kHz make between them -- a model with the shelf and no bandwidth limit
// would have no hump at all. So the hump is the evidence that both belong in
// here, and the self-test measures for it rather than taking it on trust.
//
// It gives the minimum gain as 1.5, which its own formula does not produce:
// 1 + 1M / (4.7k + 1M) is 2.0, and 2.0 is what this uses. The difference is
// 2.5 dB at the quiet end of a control whose whole travel is 40 dB, so nothing
// turns on it, but it should not go unremarked.
//
// It gives the 741's slew rate as 0.5 V/us, and that is deliberately not
// modelled. Once the bandwidth limit is in place the fastest the stage can be
// asked to move is about 0.1 V/us at the rails, so the slew limit is never the
// thing that gives way -- and a rate limiter that never engages is a branch in
// the audio path for nothing.
//
// Bias is the one control here that is not a value on the drawing -- and it is
// not a guess either, because the drawing suggests it. The note beside D1/D2
// offers "a 1n34 array like this" and draws two diodes one way against one the
// other, which is the asymmetric-clipping modification everybody who has owned
// one of these has done. Bias walks between the two: centred is the matched
// pair the pedal shipped with, and either extreme is a second diode in series
// on that side, clipping at twice the voltage and putting even harmonics in.
//
// ------------------------------------------------------- Crunch and Lead
//
// The BOSS SD-2 Dual OverDrive, from its own service notes (May 1993, First
// Edition) -- the same kind of document as the ones the rest of this
// instrument is built from, down to the measured output waveforms in the
// appendix.
//
// It is two models because it is two *circuits*. The pedal's MODE switch does
// not re-voice one chain; it picks between two complete ones, each with its
// own gain, tone and level pot section -- which is why the pots are dual-gang.
// Putting them in the Type list costs nothing and keeps a switch off a panel
// that has no room for one.
//
// CRUNCH is one stage. Op-amp 2a non-inverting, the gain leg is VR1b (250 k)
// plus R36 (10 k) over R37 (680 R) with C28 (4.7 uF) under it -- so unity at
// DC, up to 383 above a corner at 50 Hz -- and the clipper is *in the feedback
// loop*: D7 (an LED) and D6 (silicon) in series one way against D4 (an LED)
// alone the other. That asymmetry is the model, and those thresholds are
// high: about 2.2 V against 1.6 V, where a green-box overdrive clips at 0.6.
// A clipper that high stays out of the way until the playing gets loud, which
// is the whole difference between crunch and distortion.
//
// LEAD is three gain stages with two clippers between them, and it is where
// the pedal earns "dual":
//
//   3b   non-inverting, VR1a (250 k) over R28 (4.7 k) and C10 (82 n): up to
//        54, cornering at 413 Hz, so it is already voiced before it clips
//   3a   inverting, R29 (1 M) over R38 (15 k): another 67, with C20 (180 p)
//        putting a lowpass at 884 Hz across it
//   --   D14 and D15, red LEDs, *shunt* to ground behind R52 (1 k)
//   4a   inverting, and asymmetric silicon in the feedback loop: D10 one way
//        against D11 and D16 in series the other
//   4b   inverting, after two more RC sections
//
// Six thousand times the gain before the first clipper, into two clippers in
// series, each with a lowpass after it. That is why the service notes' own
// scope traces of the two modes look nothing like each other: CRUNCH keeps the
// spike and the decay of the square it is fed, and LEAD is flattened and
// ringing. The self-test drives the models with the same 200 Hz square the
// appendix specifies and checks for exactly that difference.
//
// Not modelled, and why: the JFET switches and the CMOS gates around them are
// the bypass, not the sound; the input network reduces to a 62 Hz highpass
// once the buffer in front of it is taken as ideal; and the pedal's own TONE
// stack is left at its centre because this plugin has a Tone control of its
// own sitting right after this stage, and two tone controls fighting is worse
// than one.
//
// Seven from the books, and deliberately not more.
//
// [Pirkle] table 19.2 has a hard clipper in it and it was in this list until
// it was measured: at any drive worth using it is the same spectrum as Soft
// Clip, because a soft clipper driven hard *is* a hard clipper -- the legacy
// shape clamps flat at +/-3. Two models that measure the same are one model
// with two names, which is exactly the fault this set was rebuilt to fix, so
// it went. The same goes for the rest of the sources' waveshaper tables: they
// are a dozen ways to draw one S-curve, and one S-curve is enough.

// The stage itself. One per voice; the engine owns it.
class DriveStage {
public:
   void prepare(double sampleRate);
   void reset();

   // `drive` and `bias` are 0..1 and -1..+1 as the parameters give them;
   // `mix` is 0..1, where 0 is a bypass and 1 is the stage alone.
   void setParams(int model, float drive, float bias, float mix);

   // The mix on its own. It is a crossfade and derives nothing, so the engine
   // can move it on every control block while a knob glides without paying
   // for the level matching setParams() does.
   void setMix(float mix) { mMix = clampv(mix, 0.0f, 1.0f); }

   inline float tick(float x) {
      if (mMix <= 0.0f)
         return x;
      const float wet = process(x);
      return mMix >= 1.0f ? wet : x + (wet - x) * mMix;
   }

   // The shape on its own, at the current settings and without the mix or the
   // oversampling: what the self-test measures a transfer curve with.
   float shapeOnly(float x) const;

private:
   float process(float x);

   // The waveshapers. Every one of them is an equation from the sources named
   // at the top of this file; see drive.cpp for which, and why each one is
   // shaped the way it is.
   static float softClipLegacy(float x);
   static float overdriveDafx(float x);
   float tubeDafx(float x) const;
   static float fuzzDafx(float x, float gain, float asymmetry);
   float rectifier(float x) const;
   // The node between a resistive network and a pair of diodes, solved by
   // Newton rather than shaped by a curve. `src` is the current driven into
   // the node and `g` the linear conductance away from it, so this serves a
   // shunt clipper and a feedback clipper alike -- the two differ only in what
   // is done with the answer.
   static float diodeNode(float src, float g, float isP, float vP, float isN, float vN);
   float mxrClipper(float v) const;
   float sd2Shape(float x) const;
   // The memoryless part of the circuit -- the stage's own gain, the rails and
   // the clipper -- without the filters around it.
   float germaniumShape(float x) const;
   float crush(float x) const;
   float valveStage(float x, float bias) const;

   double mSampleRate = 48000.0;
   int mModel = kDriveSoftClip;
   float mDrive = 0.2f;
   float mBias = 0.0f;
   float mMix = 1.0f;

   // Per-model gain staging and level matching. The peak is measured from the
   // model's own curve rather than assumed -- a folder's peak is in the middle
   // of its range, not at the end of it.
   float mPre = 1.0f;
   float mMakeup = 1.0f;
   float mTrim = 1.0f;

   // Tube: the work point Q and the distortion figure of [DAFX] eq 4.13, and
   // the two filters of the M-file beside it.
   float mTubeQ = -0.2f;
   float mTubeDist = 8.0f;
   float mTubeQOffset = 0.0f; // the constant term of the equation, precomputed
   OnePoleHp mTubeHp;
   OnePoleLp mTubeLp;

   // Valve Stack: four class-A triode stages, each with the DC-removing
   // highpass and the cathode-bypass low shelf of [Pirkle] 19.12, and the
   // two-band shelving EQ [Pirkle] 19.13 puts between the third and the
   // fourth. Four is the book's own NUM_TUBES.
   static constexpr int kValveStages = 4;
   OnePoleHp mValveHp[kValveStages];
   OnePoleLp mValveShelf[kValveStages];
   OnePoleLp mValveEqLow;
   OnePoleHp mValveEqHigh;
   float mValveSat = 1.0f;

   // Fuzz: the asymmetry both sources put in it.
   float mFuzzGain = 1.0f;
   float mFuzzAsym = 0.0f;

   // Rectifier: how much of the rectified signal is mixed with the straight
   // one, and the highpass that takes the offset rectification leaves behind.
   float mRectAmount = 0.0f;
   OnePoleHp mRectHp;

   // Crush.
   float mCrushStep = 1.0f / 32768.0f;
   float mCrushOffset = 0.0f;

   // Germanium: the MXR Distortion+.
   //
   // The pre-emphasis is written as a shelf rather than as a filter pair,
   // because that is what the circuit is: A(s) = 1 + (Rf/Rg) * HP(s), exactly,
   // for a one-pole highpass at 1 / (2*pi*C3*Rg). Unity at DC, Rf/Rg + 1 above
   // the corner, and the pot moves both at once.
   OnePoleHp mMxrInHp;    // C2/R1/R2
   OnePoleHp mMxrShelfHp; // the C3 leg, as the highpass half of the shelf
   OnePoleLp mMxrOpAmpLp; // the 741's gain-bandwidth limit
   OnePoleLp mMxrOutLp;   // C5 across the clipper node
   float mMxrShelfGain = 1.0f; // Rf/Rg, the amount the shelf lifts by
   float mMxrVp = 1.0f;        // the positive diode's n*Vt, times its count
   float mMxrVn = 1.0f;        // and the negative one's

   // Crunch and Lead: the SD-2's two channels. One set of members serves both
   // because the shapes are the same kind of thing in a different order; which
   // of them are used is decided by the model.
   OnePoleHp mSd2InHp;   // the input network, reduced
   OnePoleHp mSd2ShelfHp;// the gain leg's capacitor, as a shelf
   OnePoleHp mSd2Hp2;    // Lead: C18 into R38
   OnePoleLp mSd2Lp1;    // the first stage's own top-end limit
   OnePoleLp mSd2Lp2;    // Lead: C20 across R29, at 884 Hz
   OnePoleLp mSd2Lp3;    // Lead: C37 after the LEDs
   OnePoleLp mSd2Lp4;    // Lead: C41 across R59, and Crunch: C24 after R32
   float mSd2ShelfGain = 1.0f;
   float mSd2Gain2 = 1.0f;
   float mSd2Gain3 = 1.0f;
   float mSd2Gain4 = 1.0f;
   // The two clippers. Each direction carries its own saturation current and
   // its own n*Vt, because a stack of unlike diodes is not one diode scaled.
   float mSd2FbIsP = 1.0f, mSd2FbVp = 1.0f;
   float mSd2FbIsN = 1.0f, mSd2FbVn = 1.0f;

};

} // namespace substrike::dsp
