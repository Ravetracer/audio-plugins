#include "Presets.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "FactoryPresetData.h"
#include "state/Settings.h"
#include "util/Path.h"

namespace substrike {

namespace {

std::vector<std::string> splitTags(const std::string& text)
{
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        const size_t a = cur.find_first_not_of(' '), b = cur.find_last_not_of(' ');
        if (a != std::string::npos)
            out.push_back(cur.substr(a, b - a + 1));
        cur.clear();
    };
    for (char c : text)
    {
        if (c == ',')
            flush();
        else
            cur += c;
    }
    flush();
    return out;
}

int categoryRank(const std::string& c)
{
    const auto& order = factoryCategories();
    const auto it = std::find(order.begin(), order.end(), c);
    return it == order.end() ? static_cast<int>(order.size()) : static_cast<int>(it - order.begin());
}

std::string lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

const std::vector<std::string>& factoryCategories()
{
    static const std::vector<std::string> order = {
        "Init",    "Techno",    "Rumble",     "House",        "Hip Hop", "Trap", "Pop",
        "Trance",  "Psytrance", "Hardstyle",  "Hardcore",     "Drum & Bass", "Industrial",
        "Acoustic", "Lo-Fi",    "Ambient",    "Experimental", "Showcase"};
    return order;
}

PresetInfo presetInfo(const StateDocument& doc)
{
    PresetInfo p;
    auto get = [&](const char* k) {
        const auto it = doc.meta.find(k);
        return it == doc.meta.end() ? std::string() : it->second;
    };
    p.name = get("name");
    p.category = get("category");
    p.author = get("author");
    p.description = get("description");
    p.tags = splitTags(get("tags"));
    return p;
}

const std::vector<PresetInfo>& factoryPresets()
{
    static const std::vector<PresetInfo> list = [] {
        std::vector<PresetInfo> out;
        for (int i = 0; i < kNumEmbeddedPresets; ++i)
        {
            StateDocument doc;
            if (!parseState(kEmbeddedPresets[i].text, doc))
                continue;
            PresetInfo p = presetInfo(doc);
            p.key = kEmbeddedPresets[i].key;
            if (p.name.empty())
                p.name = p.key;
            p.factory = true;
            out.push_back(std::move(p));
        }
        std::stable_sort(out.begin(), out.end(), [](const PresetInfo& a, const PresetInfo& b) {
            const int ra = categoryRank(a.category), rb = categoryRank(b.category);
            if (ra != rb)
                return ra < rb;
            return lower(a.name) < lower(b.name);
        });
        return out;
    }();
    return list;
}

const PresetInfo* findFactoryPreset(const std::string& key)
{
    for (const PresetInfo& p : factoryPresets())
        if (p.key == key)
            return &p;
    return nullptr;
}

const char* factoryPresetText(const std::string& key)
{
    for (int i = 0; i < kNumEmbeddedPresets; ++i)
        if (key == kEmbeddedPresets[i].key)
            return kEmbeddedPresets[i].text;
    return nullptr;
}

std::string userPresetDir() { return fromPath(toPath(Settings::dataDir()) / "Presets"); }

bool readPresetFile(const std::string& path, StateDocument& doc)
{
    std::ifstream in(toPath(path), std::ios::binary);
    if (!in)
        return false;
    std::stringstream ss;
    ss << in.rdbuf();
    return parseState(ss.str(), doc);
}

std::vector<PresetInfo> userPresets()
{
    std::vector<PresetInfo> out;
    std::error_code ec;
    const std::filesystem::path dir = toPath(userPresetDir());
    if (!std::filesystem::is_directory(dir, ec))
        return out;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    {
        if (!e.is_regular_file(ec) || e.path().extension() != ".substrike")
            continue;
        StateDocument doc;
        const std::string path = fromPath(e.path());
        if (!readPresetFile(path, doc))
            continue;
        PresetInfo p = presetInfo(doc);
        p.key = path;
        if (p.name.empty())
            p.name = fromPath(e.path().stem());
        out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(), [](const PresetInfo& a, const PresetInfo& b) { return lower(a.name) < lower(b.name); });
    return out;
}

std::string writeUserPreset(const std::string& name, StateDocument doc)
{
    // A file name the file system accepts everywhere.
    std::string file;
    for (char c : name)
        file += std::string("<>:\"/\\|?*").find(c) == std::string::npos && static_cast<unsigned char>(c) >= 32 ? c : '_';
    if (file.empty())
        file = "Preset";
    std::error_code ec;
    const std::filesystem::path dir = toPath(userPresetDir());
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / toPath(file + ".substrike");
    doc.meta["name"] = name;
    doc.meta["category"] = "User";
    doc.meta.erase("preset");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return {};
    out << serializeState(doc);
    return out ? fromPath(path) : std::string();
}

} // namespace substrike
