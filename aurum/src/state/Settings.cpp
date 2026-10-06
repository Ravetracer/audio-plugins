#include "Settings.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "util/Path.h"

namespace aurum {

#if defined(_WIN32)

// %APPDATA%\Aurum for both: settings.ini and the Presets folder beside it.
// Read as UTF-16 so a user name outside the ANSI code page still works.
std::string Settings::configDir()
{
    const wchar_t* appData = _wgetenv(L"APPDATA");
    if (!appData || !*appData)
        return "Aurum";
    return fromPath(std::filesystem::path(appData) / "Aurum");
}

std::string Settings::dataDir() { return configDir(); }

#else

namespace {
std::string homeDir()
{
    const char* h = std::getenv("HOME");
    return h ? h : ".";
}
} // namespace

std::string Settings::configDir()
{
    const char* x = std::getenv("XDG_CONFIG_HOME");
    return (x && *x ? std::string(x) : homeDir() + "/.config") + "/Aurum";
}

std::string Settings::dataDir()
{
    const char* x = std::getenv("XDG_DATA_HOME");
    return (x && *x ? std::string(x) : homeDir() + "/.local/share") + "/Aurum";
}

#endif

Settings& Settings::get()
{
    static Settings s;
    return s;
}

Settings::Settings() : path_(configDir() + "/settings.ini") { load(); }

void Settings::load()
{
    std::ifstream in(toPath(path_));
    std::string line;
    while (std::getline(in, line))
    {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line.empty() || line[0] == '#')
            continue;
        values_[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

void Settings::save()
{
    std::error_code ec;
    std::filesystem::create_directories(toPath(configDir()), ec);
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(toPath(tmp), std::ios::trunc);
        if (!out)
            return;
        for (const auto& [k, v] : values_)
            out << k << '=' << v << '\n';
    }
    std::filesystem::rename(toPath(tmp), toPath(path_), ec);
}

std::string Settings::getString(const std::string& key, const std::string& def) const
{
    auto it = values_.find(key);
    return it == values_.end() ? def : it->second;
}

double Settings::getDouble(const std::string& key, double def) const
{
    auto it = values_.find(key);
    if (it == values_.end())
        return def;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    return end == it->second.c_str() ? def : v;
}

void Settings::set(const std::string& key, const std::string& value) { values_[key] = value; }

void Settings::set(const std::string& key, double value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", value);
    values_[key] = buf;
}

} // namespace aurum
