// SaeureKiste's window: its panels, its colours, and the filter curve across
// its header.
//
// The window itself -- the layout engine, the widgets, the preset browser, the
// typed value entry -- comes from the PluginCore shared library and lives in
// shared/src/gui/window.cpp. What is here is only what makes this window this
// plugin's.

#include <cmath>

#include "gui/seqwindow.h"

#include "params.h"
#include "saeurekiste.h"

namespace saeurekiste {

namespace {

// ------------------------------------------------------------------ geometry
//
// Cell and panel sizes are the suite's (plugincore/gui/window.h). The shape is
// not: this is the widest-for-its-height window of the lot, because the machine
// it models is a wide, shallow box with one row of knobs across it.
//
// The width is set by the widest row, which is the sequencer beside the
// generator. The height is set twice, because the window has two of them: the
// mods and the four panels that go with them live in a section that opens and
// closes, and a closed window is the page anybody actually plays from.
// Two cells wider than it was, which is what the drive stage needed: Type,
// Bias and Dist Mix on a row that was exactly full at 1180.
//
// Row 1 is now the widest rather than row 0: the sequencer, the generator and
// the delay side by side come to exactly this, with the sequencer's Mode and
// Rate stacked into one column and the generator's Seed moved out to the step
// grid where its own buttons already are. Row 0 has the slack instead.
constexpr int kContentW = 1408;

// -------------------------------------------------------------------- panels
//
// In signal order, which on this instrument is also the order of the knobs on
// the original front panel: the oscillator, the filter and its envelope, then
// the drive and the output. What is not on the front panel -- the mods, and the
// three circuits nobody re-tunes twice in a session -- is in the collapsible
// half below the preset bar.
constexpr uint32_t kVcoParams[] = {kParamWaveform, kParamTuning};
// The front panel of the machine, in the order it is printed on the lid:
// Tuning, Cutoff, Resonance, Env Mod, Decay, Accent. Tuning stays on the VCO
// panel because that is the circuit VR2 is in, and the row still reads in that
// order left to right, which is the part anybody who has played one knows by
// hand. Accent is the sixth knob and belongs here, not in a panel of its own:
// everything that *shapes* the accent is a modification and is in MODS.
constexpr uint32_t kVcfParams[] = {kParamCutoff, kParamResonance, kParamEnvMod, kParamDecay,
                                   kParamAccent};
// What reaches the filter and is not on the machine. Tracking has no circuit
// at all behind it and the other two are the Devil Fish's, so none of the
// three may sit in the front-panel row.
constexpr uint32_t kVcfModParams[] = {kParamTracking, kParamOverdrive, kParamFilterFM};
constexpr uint32_t kSlideParams[] = {kParamSlideTime};
// Type and Muffler are both chips and share a column, which is what keeps the
// panel to five: the model of the drive stage above the clipper that follows
// it. Drive and Bias sit together because they are the two controls that
// decide what the model does; Dist Mix is how much of it is heard.
constexpr uint32_t kDriveParams[] = {kParamDrive, kParamDistBias, kParamTone, kParamDistMix,
                                     kParamDistType | kStacked, kParamMuffler | kStacked};
constexpr uint32_t kOutParams[] = {kParamVolume};
// The volume envelope. Nothing on the machine reaches it at all -- its decay is
// fixed by R123 and C42 -- so the whole panel is the Devil Fish's.
constexpr uint32_t kAmpParams[] = {kParamSoftAttack, kParamAmpDecay, kParamAmpSustain};
constexpr uint32_t kSeqParams[] = {kParamMode | kStacked, kParamSeqRate | kStacked,
                                   kParamSeqSteps, kParamGate, kParamSwing};
constexpr uint32_t kVibParams[] = {kParamVibDepth, kParamVibRate, kParamVibDelay};
// Scale and Root are stacked in one column: two chips fit where one knob goes,
// and without that the generator is one column too wide to sit beside the
// sequencer on a row this window can afford.
constexpr uint32_t kRandParams[] = {
   kParamRandScale | kStacked, kParamRandRoot | kStacked, kParamRandNotes,
   kParamRandAccent,           kParamRandSlide,           kParamRandOctave,
   kParamRandVibrato,
};
// The delay. Four chips in two stacked columns, then its four knobs: the two
// that say how long and how many, and the two that say how much and how wide.
constexpr uint32_t kDelayParams[] = {
   kParamDelayOn | kStacked,   kParamDelaySync | kStacked,
   kParamDelayMode | kStacked, kParamDelayDivision | kStacked,
   kParamDelayTime,            kParamDelayFeedback,
   kParamDelayMix,             kParamDelayWidth,
};
// The mods. The numbers the schematic does not give, in the order they act:
// the filter's envelope first, then everything the accent does, then the two
// shapes and the two limits, then the machine's own unsteadiness.
//
// Accent At, Sweep Time, Sweep Speed and Accent Hold are here rather than
// beside the Accent knob because they are the same kind of thing as Acc Sweep
// and Acc Build: they say what an accent *is*, and nobody re-decides that
// while playing. The knob on the front panel says how much of it to apply.
constexpr uint32_t kModParams[] = {
   kParamEnvBias,          kParamEnvDepth,    kParamAccSweep,
   kParamAccBuild,         kParamAccGain,     kParamAccDecay,
   kParamAccentThreshold,  kParamAccentDecay, kParamSweepSpeed | kStacked,
   kParamAccentHold | kStacked, kParamDroop,  kParamLadder,
   kParamResRange,         kParamDrift,
};

#define PANEL(title, cols, rows, arr)                                                              \
   { title, cols, rows, arr, static_cast<int>(sizeof(arr) / sizeof(arr[0])) }

// The first four panels are the window that opens with the plugin; the rest
// are the section behind the ADVANCED button.
constexpr PanelSpec kPanelSpecs[] = {
   PANEL("VCO", 2, 1, kVcoParams),        PANEL("VCF", 5, 1, kVcfParams),
   PANEL("DRIVE", 5, 1, kDriveParams),    PANEL("OUTPUT", 1, 1, kOutParams),
   PANEL("SEQUENCER", 4, 1, kSeqParams),  PANEL("GENERATOR", 6, 1, kRandParams),
   PANEL("DELAY", 6, 1, kDelayParams),    PANEL("VCF MOD", 3, 1, kVcfModParams),
   PANEL("SLIDE", 1, 1, kSlideParams),    PANEL("VIBRATO", 3, 1, kVibParams),
   PANEL("AMP", 3, 1, kAmpParams),        PANEL("MODS", 13, 1, kModParams),
};
#undef PANEL

constexpr int kNumPanels = static_cast<int>(sizeof(kPanelSpecs) / sizeof(kPanelSpecs[0]));

// Which panels share a row, in order. A row is not padded out to the window's
// width: a panel is as wide as its cells and what is left over stays dark,
// because a panel stretched over empty space reads as a panel with something
// missing from it.
constexpr int kRowStart[] = {0, 4, 7, 11};
constexpr int kRowCount[] = {4, 3, 4, 1};
constexpr int kNumRows = 4;
// Rows from here on are the collapsible section, drawn below the preset bar.
constexpr int kAdvancedRow = 2;

// The bank parameters and the generator's seed are not on any panel. They are
// drawn beside the step grid, where they are used -- the seed has its own
// label and its own - and + buttons up there next to GEN, which is where
// anybody reaching for it is already looking -- and they are named here so the
// check that every parameter has a home still counts them.
constexpr uint32_t kPaneParams[] = {kParamPattern, kParamChainMode, kParamChainLength,
                                    kParamRandSeed, kParamPatternOctave};
constexpr int kNumPaneParams = static_cast<int>(sizeof(kPaneParams) / sizeof(kPaneParams[0]));

// What RESET does not touch. Everything else goes back to the default in
// params.cpp, and every one of those defaults is the stock machine -- the mods
// hold the values the engine shipped with, every Devil Fish control is at
// Whittle's own "limit it to TB-303 sounds" setting, and the drive and the
// delay are the two stages the machine does not have at all.
//
// These five are the exception because none of them is part of the sound: they
// say which of the sixty-four patterns is playing, where the notes come from
// and how fast the clock runs. A player who reaches for RESET wants the patch
// back to nothing, not the sequencer stopped and the bank back at pattern one
// in the middle of a set. The pattern's own notes are not parameters and are
// never touched by this.
constexpr uint32_t kResetKeep[] = {kParamPattern, kParamChainMode, kParamChainLength,
                                   kParamMode,    kParamSeqRate};
constexpr int kNumResetKeep = static_cast<int>(sizeof(kResetKeep) / sizeof(kResetKeep[0]));

// The layout is a table, and a table is easy to break by adding a parameter to
// a panel that has no room for it, or by forgetting to put it on a panel at
// all. None of that should need a running window to notice, so it is checked
// here instead.
constexpr int rowWidth(int row) {
   int w = 0;
   for (int i = 0; i < kRowCount[row]; ++i)
      w += panelWidth(kPanelSpecs[kRowStart[row] + i]) + (i ? kGap : 0);
   return w;
}

constexpr int rowHeight(int row) {
   int h = 0;
   for (int i = 0; i < kRowCount[row]; ++i) {
      const int ph = panelHeight(kPanelSpecs[kRowStart[row] + i]);
      h = ph > h ? ph : h;
   }
   return h;
}

// Mirrors buildLayout() in the window: the always-visible rows, the step grid,
// the preset bar, then -- only when the section is open -- the rest of the rows,
// and the help line under all of it.
constexpr int windowHeight(bool expanded) {
   int y = kHeaderH + kGap;
   for (int row = 0; row < kAdvancedRow; ++row)
      y += rowHeight(row) + kGap;
   y += kSeqPaneHeight + kGap;
   y += 2 + kBarH + 6;
   if (expanded)
      for (int row = kAdvancedRow; row < kNumRows; ++row)
         y += rowHeight(row) + kGap;
   return y - 2 + kHelpH + 8;
}

constexpr int kWindowH = windowHeight(false);
constexpr int kWindowExpandedH = windowHeight(true);

// How many columns a panel's parameters take up. A stacked pair shares one.
constexpr int panelColumnsUsed(const PanelSpec &s) {
   int n = 0;
   bool top = false;
   for (int i = 0; i < s.count; ++i) {
      const bool stack = (s.params[i] & kStacked) != 0;
      if (stack && top) {
         top = false;
         continue;
      }
      top = stack;
      ++n;
   }
   return n;
}

constexpr bool everyPanelHoldsItsParams() {
   for (int i = 0; i < kNumPanels; ++i)
      if (kPanelSpecs[i].cols * kPanelSpecs[i].rows < panelColumnsUsed(kPanelSpecs[i]))
         return false;
   return true;
}

constexpr int placedParams() {
   int n = 0;
   for (int i = 0; i < kNumPanels; ++i)
      n += kPanelSpecs[i].count;
   return n;
}

constexpr bool everyPanelIsOnARow() {
   int n = 0;
   for (int row = 0; row < kNumRows; ++row)
      n += kRowCount[row];
   return n == kNumPanels;
}

static_assert(everyPanelIsOnARow(), "kRowStart / kRowCount do not cover every panel");
static_assert(everyPanelHoldsItsParams(), "a panel has more parameters than it has cells");
static_assert(placedParams() + kNumPaneParams == static_cast<int>(kNumParams),
              "every parameter must be on exactly one panel or in kPaneParams");
static_assert(rowWidth(0) <= kContentW, "row 0 is wider than the window");
static_assert(rowWidth(1) <= kContentW, "row 1 is wider than the window");
static_assert(rowWidth(2) <= kContentW, "row 2 is wider than the window");
static_assert(rowWidth(3) <= kContentW, "row 3 is wider than the window");
static_assert(kAdvancedRow > 0 && kAdvancedRow < kNumRows,
              "the collapsible section must be some but not all of the rows");
static_assert(kWindowExpandedH > kWindowH, "opening the section has to make the window taller");

// --------------------------------------------------------------------- theme
//
// Acid green on graphite. The hue is 82 degrees -- a yellow-green that sits
// between a wasp and a leaf and is mistakable for neither -- and the chassis
// under it is a cool, almost neutral near-black rather than a tinted one,
// because the machine this models was a silver box with dark legends on it and
// a coloured chassis would be pretending it was something warmer.
constexpr Theme kTheme = {
   /* bgTop     */ {0.075, 0.079, 0.071},
   /* bgBottom  */ {0.043, 0.047, 0.043},
   /* panelFill */ {0.098, 0.102, 0.090},
   /* panelEdge */ {0.180, 0.192, 0.157},
   /* knobFace  */ {0.059, 0.063, 0.055},
   /* track     */ {0.196, 0.208, 0.176},
   /* accent    */ {0.608, 0.890, 0.114},
   /* text      */ {0.933, 0.941, 0.910},
   /* textDim   */ {0.612, 0.627, 0.573},
   /* textMute  */ {0.420, 0.435, 0.388},
   /* highlight */ {1.000, 1.000, 0.878},
};

// ------------------------------------------------------------------ ornament
//
// The filter, drawn as the thing it is.
//
// Every instrument in this family animates what it makes. What this one makes
// is a four-pole ladder being swept by an envelope, so the header draws the
// ladder's actual response curve: three poles together and a fourth an octave
// above them, with the resonant peak where the feedback puts it, and the whole
// thing sliding down the spectrum after every note exactly as the envelope
// does.
//
// The magnitude is not a decoration. It is |1 / (D + k)| with
//
//    D = (1 + j w / 1.8333 wc) (1 + j w / wc)^3
//
// which is the transfer function of the ladder in src/dsp/acid_engine.cpp with
// its unequal input capacitor in it. Widening the peak on screen would take
// changing the filter.
//
// The window cannot see inside the engine -- it is handed a note count and a
// gate, nothing more -- so where each note's sweep starts and how resonant it
// is are derived from the note number, in the way the rest of the suite derives
// an ornament from an event counter. The same note always draws the same sweep.
class FilterCurve final : public HeaderOrnament {
public:
   bool animating(uint32_t eventCounter) const override {
      return eventCounter != mLastEvent || mEnv > 0.01;
   }

   void draw(cairo_t *cr, const OrnamentContext &ctx) override {
      if (!(ctx.windowW > 1.0) || !(ctx.headerH > 1.0))
         return;

      if (ctx.eventCounter != mLastEvent) {
         mLastEvent = ctx.eventCounter;
         mEnv = 1.0;
         // A different note, a different sweep: where it starts, how far it
         // travels and how hard it rings.
         mBaseOct = 5.2 + 1.6 * unit(ctx.eventCounter * 2654435761u + 7u);
         mSweepOct = 2.6 + 2.6 * unit(ctx.eventCounter * 40503u + 13u);
         mK = 2.6 + 1.4 * unit(ctx.eventCounter * 668265263u + 19u);
      } else {
         // Roughly the machine's shortest decay at a 30 Hz repaint, so the
         // curve on screen falls at about the rate the filter does.
         mEnv *= 0.90;
         if (mEnv < 0.004)
            mEnv = 0.0;
      }

      const double h = ctx.headerH;
      const double w = ctx.windowW;
      const double top = h * 0.16;
      const double bottom = h * 0.92;

      // A decade grid, so the curve is read against something.
      setColor(cr, ctx.theme->panelEdge, 0.55);
      cairo_set_line_width(cr, 1.0);
      for (int decade = 2; decade <= 4; ++decade) {
         const double x = w * (decade - kLogLo) / (kLogHi - kLogLo);
         cairo_move_to(cr, x, top);
         cairo_line_to(cr, x, bottom);
      }
      cairo_stroke(cr);

      // Where the corner is right now. An idle header still shows a filter,
      // sitting where an untouched one would: a window with a flat line across
      // it reads as a broken graph rather than a quiet one.
      const double cutoffOct = mBaseOct + mSweepOct * mEnv;
      const double wc = 20.0 * std::pow(2.0, cutoffOct);
      const double k = mK * (0.55 + 0.45 * mEnv);

      // The curve, sampled once per couple of pixels.
      const int steps = 140;
      double firstY = bottom;
      cairo_new_path(cr);
      for (int i = 0; i <= steps; ++i) {
         const double t = static_cast<double>(i) / steps;
         const double freq = std::pow(10.0, kLogLo + t * (kLogHi - kLogLo));
         const double db = magnitudeDb(freq, wc, k);
         // -42 dB at the floor, +18 at the ceiling.
         const double norm = (db + 42.0) / 60.0;
         const double y = bottom - (bottom - top) * (norm < 0.0 ? 0.0 : (norm > 1.0 ? 1.0 : norm));
         const double x = w * t;
         if (i == 0) {
            firstY = y;
            cairo_move_to(cr, x, y);
         } else {
            cairo_line_to(cr, x, y);
         }
      }

      // Fill under it first, then stroke over the fill, so the line stays crisp.
      cairo_line_to(cr, w, bottom);
      cairo_line_to(cr, 0.0, bottom);
      cairo_line_to(cr, 0.0, firstY);
      cairo_close_path(cr);
      setColor(cr, ctx.theme->accent, 0.10 + 0.10 * mEnv);
      cairo_fill(cr);

      cairo_new_path(cr);
      for (int i = 0; i <= steps; ++i) {
         const double t = static_cast<double>(i) / steps;
         const double freq = std::pow(10.0, kLogLo + t * (kLogHi - kLogLo));
         const double db = magnitudeDb(freq, wc, k);
         const double norm = (db + 42.0) / 60.0;
         const double y = bottom - (bottom - top) * (norm < 0.0 ? 0.0 : (norm > 1.0 ? 1.0 : norm));
         const double x = w * t;
         if (i == 0)
            cairo_move_to(cr, x, y);
         else
            cairo_line_to(cr, x, y);
      }
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
      cairo_set_line_width(cr, 1.4 + 0.8 * mEnv);
      setColor(cr, ctx.theme->accent, 0.55 + 0.40 * mEnv);
      cairo_stroke(cr);

      // A mark on the corner itself, which is what the eye follows as the
      // envelope drags it down the spectrum.
      const double cx = w * (std::log10(wc) - kLogLo) / (kLogHi - kLogLo);
      if (cx > 0.0 && cx < w && mEnv > 0.01) {
         setColor(cr, ctx.theme->highlight, 0.25 + 0.55 * mEnv);
         cairo_arc(cr, cx, top + (bottom - top) * 0.10, 1.6 + 1.4 * mEnv, 0.0, 6.2831853);
         cairo_fill(cr);
      }
   }

private:
   // 30 Hz to 18 kHz, as decades.
   static constexpr double kLogLo = 1.4771; // log10(30)
   static constexpr double kLogHi = 4.2553; // log10(18000)

   // |1 / (D + k)| in dB, with D the ladder's denominator. See the comment on
   // the class: the 1.8333 is the .033 / .018 capacitor ratio off the
   // schematic, and it is why the curve is shallower just past the corner than
   // a four-pole ladder would be.
   static double magnitudeDb(double freq, double wc, double k) {
      const double a = freq / wc;
      const double b = freq / (1.8333 * wc);
      // (1 + j a)^3
      const double r3 = 1.0 - 3.0 * a * a;
      const double i3 = 3.0 * a - a * a * a;
      // times (1 + j b)
      const double dr = r3 - b * i3;
      const double di = i3 + b * r3;
      const double mag = std::sqrt((dr + k) * (dr + k) + di * di);
      return -20.0 * std::log10(mag < 1.0e-6 ? 1.0e-6 : mag);
   }

   static uint32_t hash32(uint32_t x) {
      x ^= x >> 16;
      x *= 0x7FEB352Du;
      x ^= x >> 15;
      x *= 0x846CA68Bu;
      x ^= x >> 16;
      return x;
   }
   static double unit(uint32_t x) { return (hash32(x) >> 8) * (1.0 / 16777216.0); }

   uint32_t mLastEvent = 0;
   double mEnv = 0.0;
   // Where an untouched window sits: a corner around 700 Hz with a modest peak,
   // which is roughly the middle of the cutoff knob's travel.
   double mBaseOct = 5.1;
   double mSweepOct = 3.0;
   double mK = 3.0;
};

const WindowSpec kSpec = {
   /* wordmarkFirst  */ "Säure",
   /* wordmarkSecond */ "Kiste",
   /* subtitle       */ "MONOPHONIC ACID BASS",
   /* version        */ kPluginVersion,
   /* theme          */ kTheme,
   /* panels         */ kPanelSpecs,
   /* panelCount     */ kNumPanels,
   /* rowStart       */ kRowStart,
   /* rowLength      */ kRowCount,
   /* rowCount       */ kNumRows,
   /* contentW       */ kContentW,
   /* windowH        */ kWindowH,
   /* params         */ nullptr, // filled in by createGui, paramTable() is a call
   /* paramCount     */ kNumParams,
   // One oscillator into one filter into one amplifier: there are no layers to
   // balance, so there is no mixer and no MIXER button.
   /* mixer          */ nullptr,
   /* mixerCount     */ 0,
   /* ornament       */ nullptr, // filled in by createGui: one per window
   /* ownsOrnament   */ true,
   /* pattern        */ nullptr, // filled in by createGui: it is the plugin
   /* patternSteps   */ kMaxSteps,
   /* patternCount   */ kMaxPatterns,
   /* patternParam   */ kParamPattern,
   /* chainModeParam */ kParamChainMode,
   /* chainLengthPar */ kParamChainLength,
   /* advancedRow    */ kAdvancedRow,
   /* advancedLabel  */ "ADVANCED",
   /* windowExpandedH*/ kWindowExpandedH,
   /* resetKeep      */ kResetKeep,
   /* resetKeepCount */ kNumResetKeep,
   /* host           */ nullptr, // filled in by createGui: it is the plugin too
};

} // namespace

Gui *createGui(GuiDelegate &delegate, PatternAccess &pattern, WindowHost &host) {
   // A local, and deliberately so. This used to be a function-local `static`,
   // which made it one spec for the whole module: the second instance's editor
   // overwrote the pattern and host pointers the first one was still reading,
   // so two tracks of SaeureKiste shared one pattern bank and one pattern
   // selection -- switching pattern in one switched it in the other, and a
   // pattern written in one appeared in, and vanished with, the other. The
   // window copies what it is given, so there is nothing here to keep alive.
   WindowSpec spec = kSpec;
   spec.params = paramTable();
   spec.pattern = &pattern;
   spec.host = &host;
   // One ornament per window, for the same reason: it holds the envelope of
   // the note that is sounding, and two windows sharing it would fight over
   // it. The window owns it -- see `ownsOrnament`.
   spec.ornament = new FilterCurve();
   Gui *gui = createWindow(delegate, spec);
   if (!gui)
      delete spec.ornament;
   return gui;
}

} // namespace saeurekiste
