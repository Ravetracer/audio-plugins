#pragma once

// The window is the Audio Plugins collection's; see shared/include/plugincore/gui/gui.h for
// the interface and window.h for what a plugin describes about itself. This
// only brings those names into the plugin's namespace and declares the entry
// point.

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
};

// Returns nullptr if no X display could be opened. The window reaches the
// sequencer's pattern through the second argument and the host through the
// third; the plugin is all three.
Gui *createGui(GuiDelegate &delegate, PatternAccess &pattern, WindowHost &host);

} // namespace saeurekiste
