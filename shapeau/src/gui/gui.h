#pragma once

// Shapeau's window. The parameter model, the preset browser and the Gui /
// GuiDelegate interfaces are the shared ones; the window itself is a fork of
// shared/src/gui/window.cpp (see the top of window.cpp for why), and this
// header is where the plugin and that fork meet.

#include "plugincore/gui/gui.h"
#include "plugincore/gui/window.h"

#include "gui/curveaccess.h"

namespace shapeau {

using plugincore::Gui;
using plugincore::GuiDelegate;
using plugincore::GuiPreset;

// Creates the window, or returns nullptr when no display could be opened.
Gui *createGui(GuiDelegate &delegate, CurveAccess &curves);

// The fork's own createWindow, declared in full so the fork's definition
// matches exactly. gui.cpp is its only caller.
Gui *createWindow(GuiDelegate &delegate, const plugincore::WindowSpec &spec,
                  CurveAccess &curves);

} // namespace shapeau
