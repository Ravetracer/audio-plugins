#include "PresetSession.h"

#include <filesystem>

#include "Params.h"
#include "state/Settings.h"
#include "util/Path.h"

namespace aurum {

namespace {
bool lockMix() { return Settings::get().getDouble("lock_mix", 0) > 0.5; }
} // namespace

StateDocument PresetSession::currentDocument() const
{
    StateDocument doc;
    doc.values = current_();
    // Keep existing metadata when overwriting a preset.
    if (!path_.empty())
    {
        StateDocument old;
        if (PresetManager::get().load(path_, old))
            doc.meta = old.meta;
    }
    doc.meta.erase("imported_from");
    return doc;
}

bool PresetSession::applyDocument(const StateDocument& doc, const std::string& name, const std::string& path)
{
    std::vector<double> values = doc.values;
    const int mixIdx = ParamTable::get().indexOf(pid::Mix);
    const std::vector<double> cur = current_();
    if (lockMix() && mixIdx >= 0 && static_cast<size_t>(mixIdx) < cur.size())
        values[static_cast<size_t>(mixIdx)] = cur[static_cast<size_t>(mixIdx)];
    // Bypass is a session state, not part of a sound.
    const int bypassIdx = ParamTable::get().indexOf(pid::Bypass);
    if (bypassIdx >= 0)
        values[static_cast<size_t>(bypassIdx)] = cur[static_cast<size_t>(bypassIdx)];
    apply_(values);
    loaded_ = values;
    name_ = name;
    path_ = path;
    if (onLoaded)
        onLoaded();
    return true;
}

bool PresetSession::load(const std::string& path)
{
    StateDocument doc;
    if (!PresetManager::get().load(path, doc))
        return false;
    return applyDocument(doc, fromPath(toPath(path).stem()), path);
}

bool PresetSession::saveAs(const std::string& path)
{
    StateDocument doc = currentDocument();
    if (!PresetManager::get().save(path, doc))
        return false;
    path_ = path;
    name_ = fromPath(toPath(path).stem());
    loaded_ = doc.values;
    return true;
}

bool PresetSession::saveCurrent()
{
    if (path_.empty())
        return false;
    return saveAs(path_);
}

bool PresetSession::saveAsDefault()
{
    StateDocument doc;
    doc.values = current_();
    doc.meta["description"] = "Loaded by new instances.";
    return PresetManager::get().save(PresetManager::get().defaultPresetPath(), doc);
}

void PresetSession::step(int direction)
{
    const auto& list = PresetManager::get().presets();
    if (list.empty())
        return;
    int i = PresetManager::get().indexOf(path_);
    const int n = static_cast<int>(list.size());
    i = i < 0 ? (direction > 0 ? 0 : n - 1) : (i + direction + n) % n;
    load(list[static_cast<size_t>(i)].path);
}

bool PresetSession::modified() const
{
    if (loaded_.empty())
        return false;
    const std::vector<double> cur = current_();
    const int mixIdx = ParamTable::get().indexOf(pid::Mix);
    const int bypassIdx = ParamTable::get().indexOf(pid::Bypass);
    for (size_t i = 0; i < cur.size() && i < loaded_.size(); ++i)
    {
        if (static_cast<int>(i) == bypassIdx || (lockMix() && static_cast<int>(i) == mixIdx))
            continue;
        if (std::abs(cur[i] - loaded_[i]) > 1e-6)
            return true;
    }
    return false;
}

void PresetSession::restore(const std::string& name, const std::string& path, const std::vector<double>& values)
{
    if (!name.empty())
        name_ = name;
    path_ = path;
    // Compare against the preset file itself so "modified" survives reloads.
    StateDocument doc;
    if (!path.empty() && PresetManager::get().load(path, doc))
        loaded_ = doc.values;
    else
        loaded_ = values;
}

} // namespace aurum
