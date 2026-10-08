// Parses every factory preset compiled into the plugin and reports what a
// hand-written preset can get wrong: keys the plugin does not know, values
// outside their range or not among their choices, unreadable curves, and
// missing or unknown metadata. Exit code 1 if anything is wrong.

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>

#include "FactoryPresetData.h"
#include "state/Presets.h"

using namespace substrike;

int main()
{
    int problems = 0;
    std::set<std::string> names;
    const auto& categories = factoryCategories();
    for (int i = 0; i < kNumEmbeddedPresets; ++i)
    {
        const std::string key = kEmbeddedPresets[i].key;
        StateDocument doc;
        if (!parseState(kEmbeddedPresets[i].text, doc))
        {
            std::printf("%s: not a Substrike document\n", key.c_str());
            ++problems;
            continue;
        }
        for (const std::string& w : doc.warnings)
        {
            std::printf("%s: %s\n", key.c_str(), w.c_str());
            ++problems;
        }
        const PresetInfo info = presetInfo(doc);
        for (const char* field : {"name", "category", "author", "description"})
            if (doc.meta.find(field) == doc.meta.end() || doc.meta[field].empty())
            {
                std::printf("%s: no %s\n", key.c_str(), field);
                ++problems;
            }
        if (std::find(categories.begin(), categories.end(), info.category) == categories.end())
        {
            std::printf("%s: unknown category '%s'\n", key.c_str(), info.category.c_str());
            ++problems;
        }
        if (!names.insert(info.name).second)
        {
            std::printf("%s: a second preset called '%s'\n", key.c_str(), info.name.c_str());
            ++problems;
        }
    }
    std::printf("%d factory presets, %d problem(s)\n", kNumEmbeddedPresets, problems);
    return problems == 0 ? 0 : 1;
}
