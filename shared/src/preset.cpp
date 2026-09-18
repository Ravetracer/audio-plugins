#include "plugincore/preset.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include <filesystem>

#if defined(_WIN32)
#   include <windows.h>
#else
#   include <dlfcn.h>
#endif

#include "plugincore/params.h"

namespace plugincore {

namespace {

void trim(std::string &s) {
   size_t b = 0;
   while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
      ++b;
   size_t e = s.size();
   while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
      --e;
   s = s.substr(b, e - b);
}

bool isDirectory(const std::string &path) {
   std::error_code ec;
   return !path.empty() && std::filesystem::is_directory(path, ec);
}

#if !defined(_WIN32)
// Anchor symbol: its address is inside this shared object, which lets dladdr
// report the path the host actually loaded.
void dsoAnchor() {}
#endif

// The directory the loaded plugin binary sits in. Both platforms have to ask
// the loader where it put us, because a plugin is found by the host and cannot
// assume anything about the working directory.
std::string moduleDir() {
#if defined(_WIN32)
   HMODULE self = nullptr;
   if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&isDirectory), &self))
      return {};
   wchar_t buf[MAX_PATH * 4];
   const DWORD n = GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
   if (n == 0 || n >= std::size(buf))
      return {};
   std::error_code ec;
   return std::filesystem::path(std::wstring(buf, n)).parent_path().string();
#else
   Dl_info info{};
   if (dladdr(reinterpret_cast<const void *>(&dsoAnchor), &info) == 0 || !info.dli_fname)
      return {};
   std::error_code ec;
   return std::filesystem::path(info.dli_fname).parent_path().string();
#endif
}

} // namespace

std::string factoryPresetDir(const PresetContext &ctx) {
   const std::string dir = moduleDir();
   if (dir.empty())
      return {};

   // Installed layout is <dir>/<Name>.clap + <dir>/presets, but a bundle-like
   // layout is also checked so a build tree works too. A VST3 bundle puts the
   // binary in Contents/<arch>-<os>/ and its data in Contents/Resources/, which
   // is the third candidate.
   const std::string candidates[] = {dir + "/presets", dir + "/../Resources/presets",
                                     dir + "/../presets",
                                     dir + "/" + ctx.pluginName + ".clap/presets"};
   for (const auto &c : candidates) {
      if (isDirectory(c))
         return c;
   }
   return {};
}

std::string userPresetDir(const PresetContext &ctx) {
#if defined(_WIN32)
   // Where every other Windows plugin keeps its user data.
   if (const char *appdata = std::getenv("APPDATA"))
      if (appdata[0])
         return std::string(appdata) + "\\" + ctx.pluginName + "\\presets";
   return {};
#else
   const char *xdg = std::getenv("XDG_CONFIG_HOME");
   if (xdg && xdg[0] == '/')
      return std::string(xdg) + "/" + ctx.pluginName + "/presets";
   const char *home = std::getenv("HOME");
   if (home && home[0] == '/')
      return std::string(home) + "/.config/" + ctx.pluginName + "/presets";
   return {};
#endif
}

bool parsePreset(const PresetContext &ctx, const char *text, size_t length,
                 PresetData &out, std::string &error) {
   if (!text) {
      error = "no preset data";
      return false;
   }

   out = PresetData{};
   size_t pos = 0;
   uint32_t lineNo = 0;
   bool sawAnything = false;

   while (pos < length) {
      size_t end = pos;
      while (end < length && text[end] != '\n')
         ++end;
      std::string line(text + pos, end - pos);
      pos = end + 1;
      ++lineNo;

      trim(line);
      if (line.empty() || line[0] == '#' || line[0] == ';')
         continue;

      const size_t eq = line.find('=');
      if (eq == std::string::npos) {
         char buf[128];
         std::snprintf(buf, sizeof(buf), "line %u: expected 'key = value'", lineNo);
         error = buf;
         return false;
      }

      std::string key = line.substr(0, eq);
      std::string value = line.substr(eq + 1);
      trim(key);
      trim(value);
      if (key.empty())
         continue;

      sawAnything = true;

      if (key == "name") {
         out.name = value;
      } else if (key == "author" || key == "creator") {
         out.author = value;
      } else if (key == "description") {
         out.description = value;
      } else if (key == "features" || key == "tags") {
         size_t start = 0;
         while (start <= value.size()) {
            const size_t comma = value.find(',', start);
            std::string f = value.substr(start, comma == std::string::npos ? std::string::npos
                                                                           : comma - start);
            trim(f);
            if (!f.empty())
               out.features.push_back(f);
            if (comma == std::string::npos)
               break;
            start = comma + 1;
         }
      } else if (key == "format" || key == std::string(ctx.presetExtension) + "_version") {
         // Reserved for future format revisions; version 1 is the only one.
      } else {
         const ParamDesc *desc = paramByKeyIn(ctx.table, ctx.paramCount, key.c_str());
         if (!desc)
            continue; // Unknown keys are ignored so newer presets stay loadable.

         double raw = 0.0;
         bool parsed = false;
         if (desc->kind == ParamKind::Enum) {
            for (uint32_t i = 0; i < desc->enumCount; ++i) {
               if (strcasecmp(value.c_str(), desc->enumNames[i]) == 0) {
                  raw = static_cast<double>(i);
                  parsed = true;
                  break;
               }
            }
         }
         if (!parsed) {
            char *endp = nullptr;
            const double v = std::strtod(value.c_str(), &endp);
            if (endp == value.c_str())
               continue; // not a number and not a known enum name
            // Preset files store real-world units (Hz, ms, dB, 0..1 ratios).
            raw = realToParam(*desc, v);
         }
         out.values.emplace_back(desc->id, raw);
      }
   }

   if (!sawAnything) {
      error = "preset is empty";
      return false;
   }
   return true;
}

namespace {

// Presets are meant to be read and hand-edited, so a written value is rounded to
// something a person would have typed rather than printed at full precision.
std::string formatNumber(double v) {
   char buf[64];
   if (v == std::floor(v) && std::fabs(v) < 1.0e9) {
      std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
      return buf;
   }
   const double a = std::fabs(v);
   if (a >= 100.0)
      std::snprintf(buf, sizeof(buf), "%.0f", v);
   else if (a >= 10.0)
      std::snprintf(buf, sizeof(buf), "%.1f", v);
   else if (a >= 1.0)
      std::snprintf(buf, sizeof(buf), "%.2f", v);
   else
      std::snprintf(buf, sizeof(buf), "%.3f", v);
   std::string s(buf);
   if (s.find('.') != std::string::npos) {
      while (!s.empty() && s.back() == '0')
         s.pop_back();
      if (!s.empty() && s.back() == '.')
         s.pop_back();
   }
   return s;
}

// One value per line is the whole format, so anything that could introduce a
// line of its own has to go.
std::string oneLine(const std::string &in) {
   std::string out;
   out.reserve(in.size());
   for (const char c : in)
      out.push_back((c == '\n' || c == '\r' || c == '\t') ? ' ' : c);
   std::string trimmed = out;
   trim(trimmed);
   return trimmed;
}

} // namespace

std::string formatPreset(const PresetContext &ctx, const PresetData &preset) {
   std::string out = std::string("# ") + ctx.pluginName + " preset\nformat = 1\n";
   if (!preset.name.empty())
      out += "name = " + oneLine(preset.name) + "\n";
   if (!preset.author.empty())
      out += "author = " + oneLine(preset.author) + "\n";
   if (!preset.description.empty())
      out += "description = " + oneLine(preset.description) + "\n";
   if (!preset.features.empty()) {
      out += "features = ";
      for (size_t i = 0; i < preset.features.size(); ++i) {
         if (i)
            out += ", ";
         out += oneLine(preset.features[i]);
      }
      out += "\n";
   }

   // Grouped by module, each module written once, in the order the modules first
   // appear in the parameter table. Grouping matters: parameters added later get
   // ids at the end of the table, and walking ids alone would strand them under a
   // second copy of their own heading.
   const ParamDesc *table = ctx.table;
   std::vector<const char *> modules;
   for (uint32_t i = 0; i < ctx.paramCount; ++i) {
      const char *m = table[i].module;
      bool known = false;
      for (const char *seen : modules)
         known = known || std::strcmp(seen, m) == 0;
      if (!known)
         modules.push_back(m);
   }

   for (const char *module : modules) {
      std::string body;
      for (uint32_t i = 0; i < ctx.paramCount; ++i) {
         const ParamDesc &d = table[i];
         if (std::strcmp(d.module, module) != 0)
            continue;

         bool have = false;
         double raw = 0.0;
         for (const auto &kv : preset.values) {
            if (kv.first == d.id) {
               raw = kv.second;
               have = true;
               break;
            }
         }
         if (!have)
            continue;

         body += d.key;
         body += " = ";
         if (d.kind == ParamKind::Enum && d.enumNames) {
            const long idx = static_cast<long>(raw + 0.5);
            if (idx >= 0 && idx < static_cast<long>(d.enumCount))
               body += d.enumNames[idx];
            else
               body += formatNumber(raw);
         } else {
            body += formatNumber(paramToReal(d, raw));
         }
         body += "\n";
      }
      if (body.empty())
         continue;
      out += "\n# ";
      out += module;
      out += "\n";
      out += body;
   }
   return out;
}

std::string userPresetPath(const PresetContext &ctx, const std::string &name) {
   const std::string dir = userPresetDir(ctx);
   if (dir.empty())
      return {};
   // A display name is not a filename: keep it recognisable, keep it safe.
   std::string file;
   bool lastWasDash = false;
   for (const char c : name) {
      const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_';
      if (ok) {
         file.push_back(c);
         lastWasDash = false;
      } else if (!lastWasDash && !file.empty()) {
         file.push_back('_');
         lastWasDash = true;
      }
   }
   while (!file.empty() && file.back() == '_')
      file.pop_back();
   if (file.empty())
      file = "preset";
   return dir + "/" + file + "." + ctx.presetExtension;
}

bool writePresetFile(const std::string &path, const std::string &text, std::string &error) {
   // Create every directory above the file; the plugin never assumes the user
   // preset directory already exists.
   const std::filesystem::path parent = std::filesystem::path(path).parent_path();
   if (!parent.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(parent, ec);
      if (ec && !std::filesystem::is_directory(parent)) {
         error = "cannot create " + parent.string() + ": " + ec.message();
         return false;
      }
   }

   FILE *f = std::fopen(path.c_str(), "wb");
   if (!f) {
      error = std::string("cannot write ") + path + ": " + std::strerror(errno);
      return false;
   }
   const size_t written = std::fwrite(text.data(), 1, text.size(), f);
   const bool ok = written == text.size();
   if (std::fclose(f) != 0 || !ok) {
      error = std::string("could not finish writing ") + path;
      return false;
   }
   return true;
}

bool parsePresetFile(const PresetContext &ctx, const std::string &path, PresetData &out,
                     std::string &error) {
   FILE *f = std::fopen(path.c_str(), "rb");
   if (!f) {
      error = std::string("cannot open '") + path + "': " + std::strerror(errno);
      return false;
   }
   std::string buffer;
   char chunk[4096];
   size_t n;
   while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
      buffer.append(chunk, n);
   const bool readError = std::ferror(f) != 0;
   std::fclose(f);
   if (readError) {
      error = std::string("read error on '") + path + "'";
      return false;
   }
   if (!parsePreset(ctx, buffer.data(), buffer.size(), out, error)) {
      error = path + ": " + error;
      return false;
   }
   if (out.name.empty()) {
      // Fall back to the file name without extension.
      const size_t slash = path.rfind('/');
      const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
      const size_t dot = base.rfind('.');
      out.name = dot == std::string::npos ? base : base.substr(0, dot);
   }
   return true;
}

} // namespace plugincore
