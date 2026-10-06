#pragma once

#include <map>
#include <string>

namespace aurum {

// Global user preferences shared by all instances (GUI size,
// lock mix, MIDI map, preset folder...). Stored in
// $XDG_CONFIG_HOME/Aurum/settings.ini. Main thread only.
class Settings
{
public:
    static Settings& get();

    std::string getString(const std::string& key, const std::string& def = {}) const;
    double getDouble(const std::string& key, double def) const;
    void set(const std::string& key, const std::string& value);
    void set(const std::string& key, double value);
    void save();

    static std::string configDir();
    static std::string dataDir();

private:
    Settings();
    void load();
    std::map<std::string, std::string> values_;
    std::string path_;
};

} // namespace aurum
