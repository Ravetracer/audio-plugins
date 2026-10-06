#include "PresetManager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "FfpImport.h"
#include "Settings.h"
#include "util/Path.h"

namespace fs = std::filesystem;

namespace aurum {

namespace {

bool readFile(const std::string& path, std::string& out)
{
    std::ifstream in(toPath(path), std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool writeFileAtomic(const std::string& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(toPath(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(toPath(tmp), std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << text;
        if (!out)
            return false;
    }
    fs::rename(toPath(tmp), toPath(path), ec);
    return !ec;
}

} // namespace

PresetManager& PresetManager::get()
{
    static PresetManager m;
    return m;
}

PresetManager::PresetManager()
{
    Settings& s = Settings::get();
    root_ = s.getString("preset_folder", Settings::dataDir() + "/Presets");
    favorites_ = splitTags(s.getString("favorites", ""));
    std::error_code ec;
    // Version 2: cut filters use Q 1 = Butterworth. Version 3: shelf Q scale. Version 4: predelay taper.
    // Version 5: room/predelay scales, default preset renamed to "Init".
    if (!fs::exists(toPath(root_), ec) || Settings::get().getDouble("factory_installed", 0) < 5)
    {
        // Factory files that were renamed in version 5.
        fs::remove(toPath(root_ + "/Default Setting" + kExtension), ec);
        for (const char* n : {"Vintage Hall", "Digital Chamber", "Shimmer Wash", "Lo-Fi Room"})
            fs::remove(toPath(root_ + "/05 Vintage/" + n + kExtension), ec);
        fs::remove(toPath(root_ + "/05 Vintage"), ec); // only succeeds when empty
        restoreFactory();
        s.set("factory_installed", 5.0);
        s.save();
    }
    rescan();
}

std::vector<std::string> PresetManager::splitTags(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, '|'))
    {
        const size_t a = cur.find_first_not_of(' ');
        if (a == std::string::npos)
            continue;
        cur = cur.substr(a, cur.find_last_not_of(' ') - a + 1);
        if (!cur.empty())
            out.push_back(cur);
    }
    if (out.empty() && s.find(',') != std::string::npos)
    {
        // Comma separated (imported tags).
        std::istringstream in2(s);
        while (std::getline(in2, cur, ','))
        {
            const size_t a = cur.find_first_not_of(' ');
            if (a != std::string::npos)
                out.push_back(cur.substr(a, cur.find_last_not_of(' ') - a + 1));
        }
    }
    return out;
}

std::string PresetManager::joinTags(const std::vector<std::string>& tags)
{
    std::string s;
    for (size_t i = 0; i < tags.size(); ++i)
        s += (i ? "|" : "") + tags[i];
    return s;
}

void PresetManager::setRoot(const std::string& dir)
{
    root_ = dir;
    Settings::get().set("preset_folder", dir);
    Settings::get().save();
    rescan();
}

void PresetManager::rescan()
{
    presets_.clear();
    std::error_code ec;
    const fs::path root = toPath(root_);
    if (!fs::exists(root, ec))
        return;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec || !it->is_regular_file(ec) || it->path().extension() != kExtension)
            continue;
        PresetInfo p;
        p.path = fromPath(it->path());
        fs::path rel = fs::relative(it->path(), root, ec);
        rel.replace_extension();
        p.relPath = fromPathGeneric(rel);
        p.folder = fromPathGeneric(rel.parent_path());
        p.name = fromPath(it->path().stem());
        std::string text;
        if (readFile(p.path, text))
        {
            StateDocument doc;
            if (!parseState(text, doc))
                continue;
            p.author = doc.meta["author"];
            p.description = doc.meta["description"];
            p.tags = splitTags(doc.meta["tags"]);
        }
        presets_.push_back(std::move(p));
    }
    // Folders first by name (numbers allow ordering), then presets by name.
    std::sort(presets_.begin(), presets_.end(), [](const PresetInfo& a, const PresetInfo& b) {
        if (a.folder != b.folder)
            return a.folder < b.folder;
        return a.name < b.name;
    });
}

std::vector<std::string> PresetManager::folders() const
{
    std::vector<std::string> f;
    for (const auto& p : presets_)
        if (!p.folder.empty() && (f.empty() || f.back() != p.folder))
            if (std::find(f.begin(), f.end(), p.folder) == f.end())
                f.push_back(p.folder);
    return f;
}

int PresetManager::indexOf(const std::string& path) const
{
    for (size_t i = 0; i < presets_.size(); ++i)
        if (presets_[i].path == path)
            return static_cast<int>(i);
    return -1;
}

bool PresetManager::load(const std::string& path, StateDocument& doc) const
{
    std::string text;
    if (!readFile(path, text))
        return false;
    if (parseState(text, doc))
        return true;
    // .ffp presets can be opened directly as well.
    return importFfpPreset(text, doc);
}

bool PresetManager::save(const std::string& path, const StateDocument& doc)
{
    const bool ok = writeFileAtomic(path, serializeState(doc));
    rescan();
    return ok;
}

bool PresetManager::updateMeta(const std::string& path, const std::string& author, const std::string& description,
                               const std::vector<std::string>& tags)
{
    StateDocument doc;
    if (!load(path, doc))
        return false;
    doc.meta["author"] = author;
    doc.meta["description"] = description;
    doc.meta["tags"] = joinTags(tags);
    return save(path, doc);
}

bool PresetManager::isFavorite(const std::string& relPath) const
{
    return std::find(favorites_.begin(), favorites_.end(), relPath) != favorites_.end();
}

void PresetManager::setFavorite(const std::string& relPath, bool on)
{
    auto it = std::find(favorites_.begin(), favorites_.end(), relPath);
    if (on && it == favorites_.end())
        favorites_.push_back(relPath);
    else if (!on && it != favorites_.end())
        favorites_.erase(it);
    Settings::get().set("favorites", joinTags(favorites_));
    Settings::get().save();
}

void PresetManager::restoreFactory()
{
    for (const auto& [rel, doc] : factoryPresets())
        writeFileAtomic(root_ + "/" + rel + kExtension, serializeState(doc));
    rescan();
}

int PresetManager::importFfp(const std::string& fileOrDir, std::string* lastPath)
{
    std::error_code ec;
    std::vector<fs::path> files;
    fs::path base;
    const fs::path source = toPath(fileOrDir);
    if (fs::is_directory(source, ec))
    {
        base = source;
        for (auto it = fs::recursive_directory_iterator(source, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
            if (!ec && it->is_regular_file(ec) && it->path().extension() == ".ffp")
                files.push_back(it->path());
        base = base.parent_path();
    }
    else
    {
        files.push_back(source);
        base = source.parent_path().parent_path();
    }
    int count = 0;
    for (const auto& f : files)
    {
        std::string text;
        StateDocument doc;
        if (!readFile(fromPath(f), text) || !importFfpPreset(text, doc))
            continue;
        fs::path rel = fs::relative(f, base, ec);
        if (ec || rel.empty())
            rel = f.filename();
        rel.replace_extension(kExtension);
        const std::string out = root_ + "/Imported/" + fromPathGeneric(rel);
        if (writeFileAtomic(out, serializeState(doc)))
        {
            ++count;
            if (lastPath)
                *lastPath = out;
        }
    }
    rescan();
    return count;
}

} // namespace aurum
