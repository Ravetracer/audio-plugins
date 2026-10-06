#pragma once

#include <functional>
#include <string>
#include <vector>

#include "state/PresetManager.h"

namespace aurum {

// Per-instance preset state: which preset is loaded and whether it was
// modified since. Main thread only.
class PresetSession
{
public:
    using ApplyFn = std::function<void(const std::vector<double>&)>;
    using CurrentFn = std::function<std::vector<double>()>;

    PresetSession(ApplyFn apply, CurrentFn current) : apply_(std::move(apply)), current_(std::move(current)) {}

    // Loads a preset file (.aurum or .ffp). Honours Lock Mix.
    bool load(const std::string& path);
    bool applyDocument(const StateDocument& doc, const std::string& name, const std::string& path);
    bool saveAs(const std::string& path);
    bool saveCurrent();
    bool saveAsDefault();
    void step(int direction);

    const std::string& name() const { return name_; }
    const std::string& path() const { return path_; }
    bool modified() const;

    // Plugin state integration.
    void restore(const std::string& name, const std::string& path, const std::vector<double>& values);
    void markLoaded(const std::vector<double>& values) { loaded_ = values; }

    std::function<void()> onLoaded; // e.g. push undo state

private:
    StateDocument currentDocument() const;
    ApplyFn apply_;
    CurrentFn current_;
    std::string name_ = "Init";
    std::string path_;
    std::vector<double> loaded_;
};

} // namespace aurum
