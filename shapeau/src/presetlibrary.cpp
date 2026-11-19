#include "presetlibrary.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "presets_generated.h"
#include "shapeau.h"

namespace shapeau {

using plugincore::GuiPreset;

std::vector<GuiPreset> presetLibrary() {
   std::vector<GuiPreset> presets;
   for (unsigned i = 0; i < kNumBuiltinPresets; ++i) {
      PresetData data;
      std::string error;
      if (!parsePreset(kBuiltinPresets[i].text, std::strlen(kBuiltinPresets[i].text), data, error))
         continue;
      GuiPreset entry;
      entry.name = data.name.empty() ? kBuiltinPresets[i].loadKey : data.name;
      entry.description = data.description;
      entry.loadKey = kBuiltinPresets[i].loadKey;
      entry.folder = kFactoryFolder;
      presets.push_back(entry);
   }

   const std::string dir = userPresetDir();
   std::error_code ec;
   if (dir.empty() || !std::filesystem::is_directory(dir, ec))
      return presets;
   std::vector<GuiPreset> user;
   const std::string suffix = std::string(".") + kPresetExtension;
   auto scan = [&](const std::filesystem::path &from, const std::string &folder) {
      std::error_code dirEc;
      for (const auto &entry : std::filesystem::directory_iterator(from, dirEc)) {
         if (!entry.is_regular_file())
            continue;
         const std::string name = entry.path().filename().string();
         if (name.size() <= suffix.size() ||
             name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
         const std::string path = entry.path().string();
         PresetData data;
         std::string error;
         if (!parsePresetFile(path, data, error))
            continue;
         GuiPreset item;
         item.name = data.name.empty() ? name.substr(0, name.size() - suffix.size()) : data.name;
         item.description = data.description;
         item.path = path;
         item.userContent = true;
         item.folder = folder;
         user.push_back(item);
      }
   };
   scan(dir, std::string());
   std::vector<std::string> folders;
   for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
      if (entry.is_directory())
         folders.push_back(entry.path().filename().string());
   std::sort(folders.begin(), folders.end());
   for (const auto &folder : folders)
      scan(std::filesystem::path(dir) / folder, folder);
   std::sort(user.begin(), user.end(), [](const GuiPreset &a, const GuiPreset &b) {
      return a.folder == b.folder ? a.name < b.name : a.folder < b.folder;
   });
   presets.insert(presets.end(), user.begin(), user.end());
   return presets;
}

const char *factoryPresetText(const std::string &loadKey) {
   for (unsigned i = 0; i < kNumBuiltinPresets; ++i)
      if (loadKey == kBuiltinPresets[i].loadKey)
         return kBuiltinPresets[i].text;
   return nullptr;
}

} // namespace shapeau
