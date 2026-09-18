#pragma once

// The window is the Audio Plugins collection's; see shared/include/plugincore/gui/gui.h for
// the interface and window.h for what a plugin describes about itself. This
// only brings those names into the plugin's namespace and declares the entry
// point.

#include "plugincore/gui/gui.h"

#include "pattern.h"

namespace threeohthree {

using plugincore::Gui;
using plugincore::GuiDelegate;
using plugincore::GuiPreset;

// Returns nullptr if no X display could be opened. The window reaches the
// sequencer's pattern through the second argument; the plugin is both.
Gui *createGui(GuiDelegate &delegate, PatternAccess &pattern);

} // namespace threeohthree
