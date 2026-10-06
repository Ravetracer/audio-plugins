#pragma once

#include <string>
#include <vector>

#include "StateIO.h"

namespace aurum {

struct PresetInfo
{
    std::string path;    // absolute
    std::string relPath; // relative to the preset root, without extension
    std::string folder;  // relative folder ("" for the root)
    std::string name;
    std::string author;
    std::string description;
    std::vector<std::string> tags;
};

// Preset library shared by all instances (main thread only).
// Presets are text files (*.aurum, see StateIO) in sub folders of the preset
// root; folders become categories. Favourites live in the global settings.
class PresetManager
{
public:
    static constexpr const char* kExtension = ".aurum";

    static PresetManager& get();

    std::string root() const { return root_; }
    void setRoot(const std::string& dir);
    void rescan();
    const std::vector<PresetInfo>& presets() const { return presets_; }
    std::vector<std::string> folders() const;
    int indexOf(const std::string& path) const;

    bool load(const std::string& path, StateDocument& doc) const;
    bool save(const std::string& path, const StateDocument& doc);
    bool updateMeta(const std::string& path, const std::string& author, const std::string& description,
                    const std::vector<std::string>& tags);

    bool isFavorite(const std::string& relPath) const;
    void setFavorite(const std::string& relPath, bool on);

    std::string defaultPresetPath() const { return root_ + "/Init" + kExtension; }
    void restoreFactory();

    // Converts .ffp presets (a file or a folder tree) into Aurum presets
    // under "<root>/Imported/...". Returns the number of converted files.
    int importFfp(const std::string& fileOrDir, std::string* lastPath = nullptr);

    static std::vector<std::string> splitTags(const std::string& s);
    static std::string joinTags(const std::vector<std::string>& tags);

private:
    PresetManager();
    std::string root_;
    std::vector<PresetInfo> presets_;
    std::vector<std::string> favorites_;
};

// Factory presets: (relative path, document).
std::vector<std::pair<std::string, StateDocument>> factoryPresets();

} // namespace aurum
