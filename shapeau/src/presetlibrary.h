#pragma once

// The preset library the editor's browser shows: the factory presets compiled
// into the binary, then the user's, one folder level deep. Shared by the
// plugin's window and the WCLAP's editor, which keeps the user's presets in
// the page's own file system.

#include <string>
#include <vector>

#include "plugincore/gui/gui.h"

namespace shapeau {

// The folder the factory presets are shown in.
constexpr const char *kFactoryFolder = "Factory Presets";

std::vector<plugincore::GuiPreset> presetLibrary();

// The text of a factory preset by its load key, or nullptr.
const char *factoryPresetText(const std::string &loadKey);

} // namespace shapeau
