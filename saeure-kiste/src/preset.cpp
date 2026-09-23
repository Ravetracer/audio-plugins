#include "saeurekiste.h"

#include "params.h"

#include <cctype>
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
   if (pattern)
      out += formatPattern(*pattern);
   return out;
}

} // namespace saeurekiste
