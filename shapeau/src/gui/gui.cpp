// Shapeau's window: its panels, its colours and its layout.
//
// The window itself -- the layout engine, the widgets, the preset browser, the
// typed value entry and the shape editor strip's host code -- is the fork in
// window.cpp. What is here is only what makes this window this plugin's.

#include <cmath>

#include "gui/gui.h"
#include "params.h"
#include "shapeau.h"

namespace shapeau {

namespace {

constexpr int kContentW = 1400;

// -------------------------------------------------------------------- panels
constexpr uint32_t kGlobalParams[] = {kParamMix,        kParamInput,     kParamOutput,
                                      kParamSplitLow,   kParamSplitHigh, kParamBandLow,
                                      kParamBandMid,    kParamBandHigh};
constexpr uint32_t kVolumeParams[] = {kParamVolOn, kParamVolTrim, kParamVolRate, kParamVolPhase,
                                     kParamVolSmooth};
constexpr uint32_t kFilterParams[] = {kParamFltOn,       kParamFltCutoff,  kParamFltRes,
                                      kParamFltDepth,    kParamFltDrive,   kParamFltTypeLow,
                                      kParamFltTypeMid,  kParamFltTypeHigh, kParamFltRate,
                                      kParamFltPhase,    kParamFltSmooth};
constexpr uint32_t kDriveParams[] = {kParamDrvOn,    kParamDrvStyle, kParamDrvDrive,
                                    kParamDrvInput, kParamDrvOutput, kParamDrvTone,
                                    kParamDrvMix,   kParamDrvRate,  kParamDrvPhase,
                                    kParamDrvSmooth};
constexpr uint32_t kTimeParams[] = {kParamTimOn,    kParamTimRange, kParamTimStep,
                                   kParamTimRate,  kParamTimPhase, kParamTimSmooth};
constexpr uint32_t kCrushParams[] = {kParamCrsOn,   kParamCrsAmount, kParamCrsBits,
                                    kParamCrsDown, kParamCrsMix,    kParamCrsRate,
                                    kParamCrsPhase, kParamCrsSmooth};
constexpr uint32_t kPanParams[] = {kParamPanOn, kParamPanRate, kParamPanPhase,
                                  kParamPanSmooth};
constexpr uint32_t kWidthParams[] = {kParamWidOn, kParamWidRate, kParamWidPhase,
                                    kParamWidSmooth};

#define PANEL(title, cols, rows, arr)                                                              \
   { title, cols, rows, arr, static_cast<int>(sizeof(arr) / sizeof(arr[0])) }

// The global panel, then one panel per effect in Module order -- panel 1 + m is
// effect m's. The window shows the global row and the selected effect's row,
// never more, so every row is a single panel.
constexpr PanelSpec kPanelSpecs[] = {
   PANEL("GLOBAL", 8, 1, kGlobalParams),
   PANEL("VOLUME", 5, 1, kVolumeParams),
   PANEL("FILTER", 11, 1, kFilterParams),
   PANEL("DRIVE", 10, 1, kDriveParams),
   PANEL("TIME", 6, 1, kTimeParams),
   PANEL("CRUSH", 8, 1, kCrushParams),
   PANEL("PAN", 4, 1, kPanParams),
   PANEL("WIDTH", 4, 1, kWidthParams),
};
#undef PANEL

constexpr int kNumPanels = static_cast<int>(sizeof(kPanelSpecs) / sizeof(kPanelSpecs[0]));
static_assert(kNumPanels == 1 + kNumModules, "one panel per effect, after the global one");

constexpr int kRowStart[] = {0, 1, 2, 3, 4, 5, 6, 7};
constexpr int kRowCount[] = {1, 1, 1, 1, 1, 1, 1, 1};
constexpr int kNumRows = 8;

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

// Mirrors buildLayout() in the fork: header, the shape editor, the global row
// and room for the tallest effect's row, then the preset bar and the help line.
constexpr int windowHeight() {
   int y = kHeaderH + kGap;
   y += kCurvePanelH + kGap;
   y += rowHeight(0) + kGap;
   int tallest = 0;
   for (int row = 1; row < kNumRows; ++row)
      tallest = rowHeight(row) > tallest ? rowHeight(row) : tallest;
   y += tallest + kGap;
   y += 2 + kBarH + 6;
   return y - 2 + kHelpH + 8;
}

constexpr int kWindowH = windowHeight();

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

constexpr bool everyPanelHoldsItsParams() {
   for (int i = 0; i < kNumPanels; ++i)
      if (kPanelSpecs[i].cols * kPanelSpecs[i].rows < kPanelSpecs[i].count)
         return false;
   return true;
}

static_assert(everyPanelIsOnARow(), "kRowStart / kRowCount do not cover every panel");
static_assert(everyPanelHoldsItsParams(), "a panel has more parameters than it has cells");
static_assert(placedParams() == static_cast<int>(kNumParams),
              "every parameter must be on exactly one panel");
constexpr bool everyRowFits() {
   for (int row = 0; row < kNumRows; ++row)
      if (rowWidth(row) > kContentW)
         return false;
   return true;
}
static_assert(everyRowFits(), "a panel is wider than the window");

// --------------------------------------------------------------------- theme
//
// Ice cyan at hue 189, over a graphite leaned the same way. The three existing
// plugins are acid green, signal orange and violet; a cool blue is the one
// corner of the wheel they leave alone, and a plugin whose whole idea is a
// drawn line wants a colour that reads as a line rather than as a lamp.
constexpr Theme kTheme = {
   /* bgTop     */ {0.055, 0.073, 0.081},
   /* bgBottom  */ {0.030, 0.040, 0.046},
   /* panelFill */ {0.076, 0.095, 0.104},
   /* panelEdge */ {0.153, 0.204, 0.220},
   /* knobFace  */ {0.045, 0.058, 0.065},
   /* track     */ {0.196, 0.247, 0.263},
   /* accent    */ {0.216, 0.839, 0.878},
   /* text      */ {0.925, 0.949, 0.957},
   /* textDim   */ {0.596, 0.667, 0.690},
   /* textMute  */ {0.404, 0.463, 0.482},
   /* highlight */ {0.800, 0.976, 1.000},
};

const WindowSpec kSpec = {
   /* wordmarkFirst  */ "Sha",
   /* wordmarkSecond */ "peau",
   /* subtitle       */ "MULTIBAND DRAWING RACK",
   /* version        */ kPluginVersion,
   // An effect has no voices, and the fork draws no activity meter when this is
   // null: the last row is full of controls and a strip wedged beside them
   // would only be a box with nothing in it.
   /* voiceNoun      */ nullptr,
   /* eventNoun      */ nullptr,
   /* theme          */ kTheme,
   /* panels         */ kPanelSpecs,
   /* panelCount     */ kNumPanels,
   /* rowStart       */ kRowStart,
   /* rowLength      */ kRowCount,
   /* rowCount       */ kNumRows,
   /* contentW       */ kContentW,
   /* windowH        */ kWindowH,
   /* params         */ nullptr, // filled in by createGui: paramTable() is a call
   /* paramCount     */ kNumParams,
   /* mixer          */ nullptr,
   /* mixerCount     */ 0,
   /* ornament       */ nullptr,
   /* ownsOrnament   */ false,
};

} // namespace

Gui *createGui(GuiDelegate &delegate, CurveAccess &curves) {
   // A local, not a static: one spec per window. A static spec is shared by
   // every instance in the process, which is how two tracks once ended up
   // editing one pattern bank (SäureKiste 0.3.3).
   WindowSpec spec = kSpec;
   spec.params = paramTable();
   return createWindow(delegate, spec, curves);
}

} // namespace shapeau
