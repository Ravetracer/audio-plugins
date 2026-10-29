#include "saeurekiste.h"

#include "params.h"
#include "dsp/wavetable.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <strings.h>
#include <fstream>
#include <sstream>

namespace saeurekiste {

// The preset format itself lives in shared/src/preset.cpp. All this plugin
// contributes is which name, extension and parameter table to use, and the
// wrappers that bind them so the rest of the plugin can call these without
// carrying a context around.
const PresetContext &presetContext() {
   // kPluginDirName, not kPluginName: this context is what locates the preset
   // directories, and those are named after the binary rather than after the
   // label the host shows. See saeurekiste.h.
   static const PresetContext ctx{kPluginDirName, kPresetExtension, paramTable(), kNumParams};
   return ctx;
}

std::string factoryPresetDir() { return plugincore::factoryPresetDir(presetContext()); }

std::string userPresetDir() { return plugincore::userPresetDir(presetContext()); }

std::string userWavetableDir() {
   const std::string presets = userPresetDir();
   if (presets.empty())
      return {};
   return (std::filesystem::u8path(presets).parent_path() / "wavetables").u8string();
}

bool parsePreset(const char *text, size_t length, PresetData &out, std::string &error) {
   return plugincore::parsePreset(presetContext(), text, length, out, error);
}

bool parsePresetFile(const std::string &path, PresetData &out, std::string &error) {
   return plugincore::parsePresetFile(presetContext(), path, out, error);
}

std::string formatPreset(const PresetData &preset) {
   return plugincore::formatPreset(presetContext(), preset);
}

std::string userPresetPath(const std::string &name) {
   return plugincore::userPresetPath(presetContext(), name);
}

// ---------------------------------------------------------------- the pattern
//
// The shared parser drops any key it does not recognise, so the five sequence
// lines pass straight through it untouched. This walks the same text a second
// time and picks them up.

namespace {

void trimEnds(std::string &s) {
   size_t b = 0;
   while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
      ++b;
   size_t e = s.size();
   while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
      --e;
   s = s.substr(b, e - b);
}

// The user's wavetable: its name on one line and its samples on the next, as
// "user_table = <frames> <base64>". A table that does not decode to exactly
// that many frames is dropped rather than half-loaded.
constexpr const char *kTableNameKey = "user_table_name";
constexpr const char *kTableKey = "user_table";
// What kind of preset the file is; see PresetKind. A Scene writes no line, so
// a Scene file is exactly what a preset was before kinds existed. The shared
// parser ignores the key, so an older plugin reads any of them as a Scene --
// which for a Patch or an FX chain is everything else at its default.
constexpr const char *kKindKey = "preset_kind";
const char *const kKindNames[kNumPresetKinds] = {"Scene", "Patch", "FX Chain", "Effect"};

bool parseTableLine(const std::string &key, const std::string &value, PatternData &out) {
   if (key == kKindKey) {
      for (int k = 0; k < kNumPresetKinds; ++k)
         if (strcasecmp(value.c_str(), kKindNames[k]) == 0)
            out.kind = static_cast<PresetKind>(k);
      return true;
   }
   if (key == kTableNameKey) {
      out.tableName = value;
      return true;
   }
   if (key != kTableKey)
      return false;
   const size_t space = value.find(' ');
   if (space == std::string::npos)
      return true;
   const int frames = std::atoi(value.substr(0, space).c_str());
   std::vector<int16_t> samples;
   if (frames < 1 || frames > kWtMaxFrames || !decodeTable(value.substr(space + 1), samples) ||
       samples.size() != static_cast<size_t>(frames) * kWtSourceLen)
      return true;
   out.tablePresent = true;
   out.tableFrames = frames;
   out.table = std::move(samples);
   return true;
}

void scanPatternLines(const char *text, size_t length, PatternData &out) {
   std::string all(text, length);
   std::istringstream in(all);
   std::string line;
   while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r')
         line.pop_back();
      std::string trimmed = line;
      trimEnds(trimmed);
      if (trimmed.empty() || trimmed[0] == '#')
         continue;
      const size_t eq = trimmed.find('=');
      if (eq == std::string::npos)
         continue;
      std::string key = trimmed.substr(0, eq);
      std::string value = trimmed.substr(eq + 1);
      trimEnds(key);
      trimEnds(value);
      if (!parseTableLine(key, value, out))
         parsePatternLine(key, value, out);
   }
}

} // namespace

bool parsePreset(const char *text, size_t length, PresetData &out, PatternData *pattern,
                 std::string &error) {
   if (!plugincore::parsePreset(presetContext(), text, length, out, error))
      return false;
   if (pattern && text)
      scanPatternLines(text, length, *pattern);
   return true;
}

bool parsePresetFile(const std::string &path, PresetData &out, PatternData *pattern,
                     std::string &error) {
   if (!pattern)
      return parsePresetFile(path, out, error);
   std::ifstream file(path, std::ios::binary);
   if (!file) {
      error = "cannot open '" + path + "'";
      return false;
   }
   std::ostringstream buf;
   buf << file.rdbuf();
   const std::string text = buf.str();
   if (!parsePreset(text.c_str(), text.size(), out, pattern, error)) {
      error = path + ": " + error;
      return false;
   }
   return true;
}

std::string formatPreset(const PresetData &preset, const PatternData *pattern) {
   std::string out = plugincore::formatPreset(presetContext(), preset);
   if (pattern) {
      if (pattern->kind != PresetKind::Scene)
         out += std::string(kKindKey) + " = " + kKindNames[static_cast<int>(pattern->kind)] + "\n";
      out += formatPattern(*pattern);
      if (pattern->tablePresent && !pattern->table.empty()) {
         std::string name = pattern->tableName;
         for (char &c : name)
            if (c == '\n' || c == '\r')
               c = ' ';
         out += std::string(kTableNameKey) + " = " + name + "\n";
         out += std::string(kTableKey) + " = " + std::to_string(pattern->tableFrames) + " " +
                encodeTable(pattern->table) + "\n";
      }
   }
   return out;
}

} // namespace saeurekiste
