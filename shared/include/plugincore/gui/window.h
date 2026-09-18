#pragma once

// What a plugin has to say about itself for the shared window to draw it.
//
// The window itself -- the layout engine, the knobs, the chips and dropdowns,
// the preset browser, the save field, the typed value entry, the meters -- is
// the same in every plugin built on it and lives in shared/src/gui/window.cpp.
// What differs between plugins is described here: the palette, the panels, the
// wordmark, and whatever the header animates.

#include <cstdint>

#include <cairo/cairo.h>

#include "plugincore/gui/gui.h"
#include "plugincore/gui/toolkit.h"
#include "plugincore/params.h"

namespace plugincore {

// ------------------------------------------------------------------- theme
//
// Every plugin in the suite has its own colour theme. The window is otherwise
// identical, so the palette is what tells them apart at a glance: RainyDay's
// rain blue, ThunderClap's lightning violet.
//
// The greys are deliberately part of the theme rather than fixed. A plugin
// tints them towards its own accent, which is what makes the whole window feel
// like one instrument instead of a grey chassis with a coloured knob.
struct Theme {
   Rgb bgTop;
   Rgb bgBottom;
   Rgb panelFill;
   Rgb panelEdge;
   Rgb knobFace;
   Rgb track;
   Rgb accent;
   Rgb text;
   Rgb textDim;
   Rgb textMute;
   // Used by a header ornament that wants a second, brighter colour -- the
   // white-hot core of a lightning channel, for instance. Defaults to `text`
   // when a plugin does not set it.
   Rgb highlight;
};

// ------------------------------------------------------------------ layout

// One parameter's place in a panel's grid. A cell either holds a knob or, for
// enum parameters, a chip that is clicked on its left or right half to step
// through the choices.
struct Cell {
   uint32_t param;
   int col;
   int row;
   int span;
};

struct PanelSpec {
   const char *title;
   int cols;
   int rows;
   const uint32_t *params;
   int count;
};

// Sizes shared by every plugin, in design pixels. A display scale is applied by
// cairo when painting, so nothing here has to know about it. These are fixed
// across the suite on purpose: a knob is the same size in every plugin.
constexpr int kCellW = 84;
constexpr int kCellH = 96;
constexpr int kPanelPad = 8;
constexpr int kPanelTitleH = 24;
constexpr int kMargin = 16;
constexpr int kGap = 8;
constexpr int kHeaderH = 66;
constexpr int kBarH = 32;
constexpr int kHelpH = 24;
constexpr double kMenuRowH = 20.0;
constexpr double kMenuPad = 4.0;
constexpr double kKnobR = 21.0;
constexpr double kArcStart = 0.75 * 3.14159265358979323846;
constexpr double kArcSweep = 1.5 * 3.14159265358979323846;

// Compile-time layout checks a plugin can run over its own panel table. Kept
// here so every plugin gets the same ones; kept constexpr so a layout mistake
// is a build error rather than something to notice in a running window.
constexpr int panelWidth(const PanelSpec &s) { return s.cols * kCellW + 2 * kPanelPad; }
constexpr int panelHeight(const PanelSpec &s) {
   return kPanelTitleH + s.rows * kCellH + kPanelPad;
}

// ------------------------------------------------------------------- mixer
//
// The layer mixer. Every plugin in the suite layers several generators -- a
// far-field bed under close drops, a swell under foam under wash -- and each
// layer's level sits on whichever panel that layer belongs to. That is right
// for editing one layer and wrong for balancing them against each other, which
// is what building a preset from scratch mostly is: pull everything else down,
// get one layer right, bring the next one back.
//
// So a plugin lists its layers here and the shared window draws a mixer for
// them: one strip per layer, with the layer's level as a fader and its
// placement beside it. The parameters are the plugin's own -- the mixer is a
// second view of controls that also live on the panels, not a set of new ones.
//
// A plugin with nothing to mix leaves `mixer` null and gets no MIXER button.
constexpr uint32_t kNoParam = 0xFFFFFFFFu;

struct MixerStrip {
   const char *label; // the layer's name, e.g. "DISTANT", "TRICKLE"
   uint32_t level;    // its level parameter; the fader
   uint32_t pan;      // its pan, or kNoParam when the layer cannot be placed
   uint32_t width;    // its stereo width, or kNoParam
   // The whole instrument's output rather than one layer. Drawn after a gap
   // and given no mute or solo, because soloing the master would only silence
   // everything else.
   bool master;
};

// ---------------------------------------------------------------- ornament

// What the header animates behind the wordmark. RainyDay runs rain streaks
// whose density follows the voice load; ThunderClap grows a lightning bolt from
// each flash. A plugin with nothing to animate simply does not supply one.
struct OrnamentContext {
   double windowW;
   double headerH;
   const Theme *theme;
   double voiceLoad;       // voices sounding / the pool, 0..1
   uint32_t eventCounter;  // guiEventCounter(), monotonic
};

class HeaderOrnament {
public:
   virtual ~HeaderOrnament() = default;

   // Whether the ornament has something to animate this frame. Checked before
   // drawing, and it must not change any state: the window uses it to decide
   // whether to repaint at all.
   virtual bool animating(uint32_t eventCounter) const {
      (void)eventCounter;
      return false;
   }

   // Drawn behind the wordmark, already clipped to the header rectangle. May
   // advance the ornament's own animation state.
   virtual void draw(cairo_t *cr, const OrnamentContext &ctx) = 0;
};

// -------------------------------------------------------------------- spec

struct WindowSpec {
   // Wordmark: drawn as two halves, the second in the accent colour --
   // "Rainy" + "Day", "Thunder" + "Clap".
   const char *wordmarkFirst;
   const char *wordmarkSecond;
   const char *subtitle; // e.g. "SYNTHETIC RAIN INSTRUMENT", drawn right-aligned
   const char *version;  // the plugin's kPluginVersion

   // The activity meter counts these, e.g. "droplets", "shocks".
   const char *voiceNoun;
   // A second count shown beside it, e.g. "flashes". Null if the plugin has
   // no discrete events.
   const char *eventNoun;

   Theme theme;

   // The panel table, and which panels share a row.
   const PanelSpec *panels;
   int panelCount;
   const int *rowStart;
   const int *rowLength;
   int rowCount;

   // Content width in design pixels; the window is this plus two margins. The
   // height is fixed by the plugin because its panels have to fit.
   int contentW;
   int windowH;

   // The plugin's parameter table.
   const ParamDesc *params;
   uint32_t paramCount;

   // The layer mixer, or null when a plugin has no layers worth mixing.
   const MixerStrip *mixer;
   int mixerCount;

   // Optional; nothing is drawn behind the wordmark when it is null.
   HeaderOrnament *ornament;
};

// Creates the window. Returns nullptr if no X display could be opened. `spec`
// must outlive the window.
Gui *createWindow(GuiDelegate &delegate, const WindowSpec &spec);

} // namespace plugincore
