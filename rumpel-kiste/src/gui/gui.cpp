// RumpelKiste's window: its panels, its colours, and the running light across
// its header.
//
// The window itself -- the layout engine, the widgets, the preset browser, the
// typed value entry, the step grid and the bank -- is the fork in
// seqwindow.cpp. What is here is only what makes this window this plugin's.

#include <cmath>

#include "gui/seqwindow.h"

#include "params.h"
#include "rumpelkiste.h"

namespace rumpelkiste {

namespace {

// ------------------------------------------------------------------ geometry
//
// Cell and panel sizes are the suite's. The shape follows the machine: a long,
// low box with every voice's knobs in one row across the top of it, and the
// sequencer underneath. The top row is the widest -- five voices, seventeen
// knobs -- and sets the window's width.
constexpr int kContentW = 1540;

// -------------------------------------------------------------------- panels
//
// The front panel, left to right as the machine prints it.
constexpr uint32_t kBdParams[] = {kParamBdTune, kParamBdLevel, kParamBdAttack, kParamBdDecay};
constexpr uint32_t kSdParams[] = {kParamSdTune, kParamSdLevel, kParamSdTone, kParamSdSnappy};
constexpr uint32_t kLtParams[] = {kParamLtTune, kParamLtLevel, kParamLtDecay};
constexpr uint32_t kMtParams[] = {kParamMtTune, kParamMtLevel, kParamMtDecay};
constexpr uint32_t kHtParams[] = {kParamHtTune, kParamHtLevel, kParamHtDecay};
constexpr uint32_t kRcParams[] = {kParamRsLevel, kParamCpLevel};
constexpr uint32_t kHhParams[] = {kParamHhLevel, kParamChDecay, kParamOhDecay};
constexpr uint32_t kCrParams[] = {kParamCrLevel, kParamCrTune};
constexpr uint32_t kRdParams[] = {kParamRdLevel, kParamRdTune};
// Mode and Scale are both chips and share a column.
constexpr uint32_t kSeqParams[] = {kParamMode | kStacked, kParamScale | kStacked, kParamSteps,
                                   kParamShuffle, kParamFlam};
constexpr uint32_t kMasterParams[] = {kParamAccent, kParamVolume};

// The generator's settings, and behind the ADVANCED button the mods -- the
// numbers the service notes do not give, split by what they belong to.
constexpr uint32_t kGenParams[] = {kParamGenStyle, kParamGenBusy, kParamGenAccent};
constexpr uint32_t kVoiceModParams[] = {kParamBdPitch,  kParamBdSweep,  kParamBdShape,
                                        kParamSdPitch,  kParamTomPitch, kParamTomSweep,
                                        kParamTomNoise, kParamRsDecay,  kParamCpSpread};
constexpr uint32_t kMetalParams[] = {kParamHatColor, kParamCymColor, kParamDacBits};
// The drive bus, visible with the rest of the front panel: it changes what
// the instrument is, not how one voice is tuned. The route switches are chips
// stacked two to a column -- eleven of them fit in six.
constexpr uint32_t kDriveParams[] = {kParamDriveMode | kStacked, kParamDistType | kStacked,
                                     kParamDrive, kParamDistBias, kParamDriveTone, kParamDistMix};
// Paired so the top row reads as the drums, left to right as the front panel
// has them, and the bottom row as rim, clap and the metal.
constexpr uint32_t kRouteParams[] = {
   kParamRouteBd | kStacked, kParamRouteRs | kStacked, kParamRouteSd | kStacked,
   kParamRouteCp | kStacked, kParamRouteLt | kStacked, kParamRouteCh | kStacked,
   kParamRouteMt | kStacked, kParamRouteOh | kStacked, kParamRouteHt | kStacked,
   kParamRouteCr | kStacked, kParamRouteRd | kStacked};
constexpr uint32_t kFeelParams[] = {kParamLocalAccent, kParamAccentThreshold, kParamShuffleUnit,
                                    kParamFlamUnit, kParamFlamGrace};

#define PANEL(title, cols, rows, arr)                                                              \
   { title, cols, rows, arr, static_cast<int>(sizeof(arr) / sizeof(arr[0])) }

constexpr PanelSpec kPanelSpecs[] = {
   PANEL("BASS DRUM", 4, 1, kBdParams),   PANEL("SNARE DRUM", 4, 1, kSdParams),
   PANEL("LOW TOM", 3, 1, kLtParams),     PANEL("MID TOM", 3, 1, kMtParams),
   PANEL("HI TOM", 3, 1, kHtParams),

   PANEL("RIM / CLAP", 2, 1, kRcParams),  PANEL("HI-HAT", 3, 1, kHhParams),
   PANEL("CRASH", 2, 1, kCrParams),       PANEL("RIDE", 2, 1, kRdParams),
   PANEL("SEQUENCER", 4, 1, kSeqParams),  PANEL("MASTER", 2, 1, kMasterParams),

   PANEL("DRIVE", 5, 1, kDriveParams),    PANEL("DRIVE ROUTE", 6, 1, kRouteParams),
   PANEL("GENERATOR", 3, 1, kGenParams),

   PANEL("VOICE MODS", 9, 1, kVoiceModParams), PANEL("METAL", 3, 1, kMetalParams),
   PANEL("FEEL", 5, 1, kFeelParams),
};
#undef PANEL

constexpr int kNumPanels = static_cast<int>(sizeof(kPanelSpecs) / sizeof(kPanelSpecs[0]));

constexpr int kRowStart[] = {0, 5, 11, 14};
constexpr int kRowCount[] = {5, 6, 3, 3};
constexpr int kNumRows = 4;
// The drive and the generator are on the page that opens with the plugin; the
// mods are behind ADVANCED.
constexpr int kAdvancedRow = 3;

// Not on any panel: drawn beside the grid, where they are used.
constexpr uint32_t kPaneParams[] = {kParamPattern,        kParamChainMode,   kParamChainLength,
                                    kParamPatternTrigger, kParamChainRepeat, kParamGenSeed};
constexpr int kNumPaneParams = static_cast<int>(sizeof(kPaneParams) / sizeof(kPaneParams[0]));

// What RESET does not touch: which pattern plays, how the bank chains, and
// how the clock runs. A player who reaches for RESET wants the kit back to the
// factory, not the song stopped and the bank back at pattern one.
constexpr uint32_t kResetKeep[] = {kParamPattern,    kParamChainMode,      kParamChainLength,
                                   kParamMode,       kParamScale,          kParamSteps,
                                   kParamPatternTrigger, kParamChainRepeat};
constexpr int kNumResetKeep = static_cast<int>(sizeof(kResetKeep) / sizeof(kResetKeep[0]));

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

// Mirrors buildLayout() in the window.
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
static_assert(kWindowExpandedH > kWindowH, "opening the section has to make the window taller");

// --------------------------------------------------------------------- theme
//
// Signal orange at hue 22, over a warm graphite. The machine is a grey box
// whose step keys run from red through orange and yellow to white, and orange
// is the one of those that reads on a dark screen as a lamp rather than an
// alarm. The greys lean the same way, a few degrees warm, so the window reads
// as one instrument; SäureKiste's are cool, and the two should not be
// mistaken for each other across a room.
constexpr Theme kTheme = {
   /* bgTop     */ {0.082, 0.074, 0.070},
   /* bgBottom  */ {0.047, 0.043, 0.041},
   /* panelFill */ {0.106, 0.096, 0.090},
   /* panelEdge */ {0.205, 0.176, 0.158},
   /* knobFace  */ {0.064, 0.058, 0.055},
   /* track     */ {0.220, 0.192, 0.175},
   /* accent    */ {1.000, 0.431, 0.102},
   /* text      */ {0.945, 0.925, 0.910},
   /* textDim   */ {0.645, 0.605, 0.580},
   /* textMute  */ {0.445, 0.412, 0.393},
   /* highlight */ {1.000, 0.930, 0.780},
};

// ------------------------------------------------------------------ ornament
//
// The running light. The machine has sixteen LEDs over its step keys and one
// of them walks across while a pattern plays; this is those sixteen, walking
// one place for every hit the engine plays and fading behind, so a busy
// pattern leaves a comet and a sparse one a spark.
class RunningLight final : public HeaderOrnament {
public:
   bool animating(uint32_t eventCounter) const override {
      if (eventCounter != mLastEvent)
         return true;
      for (double b : mGlow)
         if (b > 0.01)
            return true;
      return false;
   }

   void draw(cairo_t *cr, const OrnamentContext &ctx) override {
      if (!(ctx.windowW > 1.0) || !(ctx.headerH > 1.0))
         return;
      if (ctx.eventCounter != mLastEvent) {
         uint32_t steps = ctx.eventCounter - mLastEvent;
         mLastEvent = ctx.eventCounter;
         if (steps > 16)
            steps = 16;
         for (uint32_t i = 0; i < steps; ++i) {
            mLit = (mLit + 1) % 16;
            mGlow[mLit] = 1.0;
         }
      }
      const double x0 = ctx.windowW * 0.36;
      const double x1 = ctx.windowW * 0.76;
      const double pitch = (x1 - x0) / 16.0;
      const double w = pitch * 0.62;
      const double h = 6.0;
      const double y = ctx.headerH * 0.52 - h * 0.5;
      for (int i = 0; i < 16; ++i) {
         const double x = x0 + i * pitch + (pitch - w) * 0.5;
         roundedRect(cr, x, y, w, h, 2.0);
         setColor(cr, ctx.theme->panelEdge, 0.55);
         cairo_fill(cr);
         if (mGlow[i] > 0.01) {
            roundedRect(cr, x, y, w, h, 2.0);
            setColor(cr, i % 4 == 0 ? ctx.theme->highlight : ctx.theme->accent,
                     0.15 + 0.8 * mGlow[i]);
            cairo_fill(cr);
         }
         mGlow[i] *= 0.84;
         if (mGlow[i] < 0.005)
            mGlow[i] = 0.0;
      }
   }

private:
   uint32_t mLastEvent = 0;
   int mLit = 15;
   double mGlow[16] = {0};
};

const WindowSpec kSpec = {
   /* wordmarkFirst  */ "Rumpel",
   /* wordmarkSecond */ "Kiste",
   /* subtitle       */ "RHYTHM COMPOSER",
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
   // The voices each have a Level on their own panel, and all eleven are in
   // one row of knobs already: a mixer would only draw them again.
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
   /* repeatParam    */ kParamChainRepeat,
   /* triggerParam   */ kParamPatternTrigger,
   /* advancedRow    */ kAdvancedRow,
   /* advancedLabel  */ "ADVANCED",
   /* windowExpandedH*/ kWindowExpandedH,
   /* resetKeep      */ kResetKeep,
   /* resetKeepCount */ kNumResetKeep,
   /* host           */ nullptr, // filled in by createGui: it is the plugin too
};

} // namespace

Gui *createGui(GuiDelegate &delegate, PatternAccess &pattern, WindowHost &host) {
   // A local, not a static: one spec per window, for the reason SäureKiste
   // learned the hard way -- a static spec is shared by every instance in the
   // process, and two tracks end up editing one bank.
   WindowSpec spec = kSpec;
   spec.params = paramTable();
   spec.pattern = &pattern;
   spec.host = &host;
   spec.ornament = new RunningLight();
   Gui *gui = createWindow(delegate, spec);
   if (!gui)
      delete spec.ornament;
   return gui;
}

} // namespace rumpelkiste
