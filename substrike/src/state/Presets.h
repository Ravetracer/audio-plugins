#pragma once

#include <string>
#include <vector>

#include "state/StateIO.h"

namespace substrike {

// A preset is a state document (see StateIO.h) whose [Substrike] section
// carries its metadata:
//
//   name=Concrete Thump
//   category=Techno
//   author=Substrike
//   tags=punchy, distorted
//   description=One sentence a musician can read.
//
// The factory presets are presets/*.substrike, compiled into the binary;
// their load key is the file name without the extension. The user's are
// files in userPresetDir().
struct PresetInfo
{
    std::string key;  // factory: the load key; user: the file path
    std::string name;
    std::string category;
    std::string author;
    std::string description;
    std::vector<std::string> tags;
    bool factory = false;
};

// The factory presets, in the order of factoryCategories() and then by name.
const std::vector<PresetInfo>& factoryPresets();
const PresetInfo* findFactoryPreset(const std::string& key);
// The categories the factory presets use, in the order a browser lists them.
const std::vector<std::string>& factoryCategories();
// The text of a factory preset, or null.
const char* factoryPresetText(const std::string& key);

// $XDG_DATA_HOME/Substrike/Presets, %APPDATA%\Substrike\Presets on Windows.
std::string userPresetDir();
// The user's presets, by name. Reads the folder; main thread.
std::vector<PresetInfo> userPresets();
// Reads a preset (or state) file. Paths are UTF-8.
bool readPresetFile(const std::string& path, StateDocument& doc);
// Writes `doc` as a user preset called `name`, creating the folder; returns
// the path, or empty on failure.
std::string writeUserPreset(const std::string& name, StateDocument doc);

// The metadata of a parsed document.
PresetInfo presetInfo(const StateDocument& doc);

} // namespace substrike
