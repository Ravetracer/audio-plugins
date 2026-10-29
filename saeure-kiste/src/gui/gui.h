#pragma once

// The window is the Audio Plugins collection's; see shared/include/plugincore/gui/gui.h for
// the interface and window.h for what a plugin describes about itself. This
// only brings those names into the plugin's namespace and declares the entry
// point.

#include <cstdint>
#include <string>
#include <vector>

#include "plugincore/gui/gui.h"

#include "pattern.h"

namespace saeurekiste {

using plugincore::Gui;
using plugincore::GuiDelegate;
using plugincore::GuiPreset;

// What the window needs from the plugin that is neither a parameter nor a
// pattern: a way to ask the host for a different size, and somewhere to keep
// the one bit of window state that has to outlive the window.
//
// The collapsible panel section changes the window's height, and a CLAP editor
// does not get to resize itself -- it asks, and the host either obliges or does
// not. So the toggle lays the window out at its new size and then calls this,
// and the plugin turns it into clap_host_gui::request_resize.
// One row of the wavetable selector.
struct TableRow {
   enum class Kind {
      Heading, // BUILT-IN, USER: a label, not a choice
      Folder,  // a folder in the user's: opens and closes what is under it
      Table,   // a table to pick
      Note,    // a line of explanation, such as where the folder is
   };
   Kind kind = Kind::Table;
   std::string label;
   std::string key;    // what windowSelectTable() takes; a folder's own path
   int depth = 0;      // indentation
   bool current = false;
};

class WindowHost {
public:
   virtual ~WindowHost() = default;

   // The layout's size changed. Both are pixels at the window's current scale.
   virtual void windowRequestResize(uint32_t width, uint32_t height) = 0;

   // Whether the collapsible section is open. Kept by the plugin so that
   // closing and reopening the editor, or reloading the project, finds it the
   // way it was left.
   virtual bool windowAdvancedOpen() const = 0;
   virtual void windowSetAdvancedOpen(bool open) = 0;

   // The oscillator display: one cycle of what the oscillator is making.
   //
   // A key that changes whenever that cycle would -- the waveform, the table,
   // the waveform's own control with any modulation the host is applying, a
   // newly loaded table -- so the window repaints when the picture moves and
   // not otherwise.
   virtual uint64_t windowWaveKey() const = 0;
   // `n` samples of the cycle, and a short caption naming what it is.
   virtual void windowWaveCycle(float *out, int n, std::string &caption) = 0;
   // The parameter the display edits: the control that belongs to the current
   // waveform (Pulse Width, WT Position, ...), or kNoParam for the sawtooth and
   // the square, which have none.
   virtual uint32_t windowWaveParam() const = 0;

   // The wavetable selector. The rows are built fresh each time the list
   // opens, so a file dropped into the folder shows up the next time.
   virtual void windowTableRows(std::vector<TableRow> &rows) = 0;
   // Selects a row's table (its `key`) and switches the waveform to
   // Wavetable. False, with a reason, if a file could not be read.
   virtual bool windowSelectTable(const std::string &key, std::string &error) = 0;
   // The previous or next table: the built-in ones, then the folder's in the
   // order the list shows them.
   virtual void windowStepTable(int direction) = 0;
   // What the Table chip shows: a built-in table's name or the user file's.
   virtual std::string windowTableLabel() const = 0;
   // The user's wavetable folder, created if it does not exist yet.
   virtual std::string windowTableFolder() = 0;
   // Reads any WAV file as the user's wavetable and selects it.
   virtual bool windowLoadWavetable(const std::string &path, std::string &error) = 0;

   // The FX page. The window shows the slots that hold an effect, in slot
   // order, and these change which ones those are. Adding fills the slot after
   // the last effect, removing closes the gap, and moving swaps an effect with
   // its neighbour, so the page never has a hole in it to draw. `slot` is a
   // slot index, not a place on the page.
   virtual void windowFxAdd() = 0;
   virtual void windowFxRemove(int slot) = 0;
   virtual void windowFxMove(int slot, int delta) = 0;

   // The three preset fields beside the shared Scene one: PATCH on the main
   // page, the FX chain on the FX page, and one per FX strip for its Effect.
   // Every preset in guiPresets() is one PresetKind; `slot` is only read for
   // an Effect.
   virtual PresetKind windowPresetKind(int index) const = 0;
   // What a field shows: an index into guiPresets(), or -1 for nothing.
   virtual int windowCurrentPreset(PresetKind kind, int slot) const = 0;
   virtual bool windowPresetEdited(PresetKind kind, int slot) const = 0;
   // Loads preset `index` as `kind` -- a Scene loaded as a Patch gives up only
   // its sound and its patterns, and an Effect goes into `slot`.
   virtual void windowLoadPresetAs(int index, PresetKind kind, int slot) = 0;
   // SAVE for a field. "Folder/Name" picks a folder as it does for a Scene; a
   // bare name goes to the kind's own shelf. False, with a reason, on failure.
   virtual bool windowSavePresetAs(PresetKind kind, int slot, const std::string &name,
                                   std::string &error) = 0;
   virtual std::string windowSuggestedName(PresetKind kind, int slot) const = 0;
};

// Returns nullptr if no X display could be opened. The window reaches the
// sequencer's pattern through the second argument and the host through the
// third; the plugin is all three.
Gui *createGui(GuiDelegate &delegate, PatternAccess &pattern, WindowHost &host);

} // namespace saeurekiste
