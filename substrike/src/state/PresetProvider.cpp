// Adapted from the PluginCore library's provider (shared/src/preset_provider.cpp)
// for Substrike's own preset format. Substrike does not link shared/.

#include "PresetProvider.h"

#include <cstring>
#include <filesystem>
#include <string>

#include "state/Presets.h"
#include "substrike.h"
#include "util/Path.h"

namespace substrike {

namespace {

constexpr const char* kProviderId = "de.ravetracer.substrike.preset-provider";

const clap_preset_discovery_provider_descriptor_t kDescriptor = {CLAP_VERSION_INIT, kProviderId,
                                                                 "Substrike Presets", "Ravetracer"};
const clap_universal_plugin_id_t kUniversalId = {"clap", kPluginId};

struct Provider
{
    clap_preset_discovery_provider_t iface{};
    const clap_preset_discovery_indexer_t* indexer = nullptr;
    std::string userDir;

    static Provider* from(const clap_preset_discovery_provider_t* p) { return static_cast<Provider*>(p->provider_data); }
};

bool describe(const clap_preset_discovery_metadata_receiver_t* rx, const PresetInfo& p, const char* loadKey,
              bool factory)
{
    if (!rx->begin_preset(rx, p.name.c_str(), loadKey))
        return false;
    rx->add_plugin_id(rx, &kUniversalId);
    rx->set_flags(rx, factory ? CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT : CLAP_PRESET_DISCOVERY_IS_USER_CONTENT);
    if (!p.author.empty())
        rx->add_creator(rx, p.author.c_str());
    if (!p.description.empty())
        rx->set_description(rx, p.description.c_str());
    rx->add_feature(rx, CLAP_PLUGIN_FEATURE_INSTRUMENT);
    rx->add_feature(rx, CLAP_PLUGIN_FEATURE_DRUM);
    rx->add_feature(rx, "kick");
    if (!p.category.empty())
        rx->add_feature(rx, p.category.c_str());
    for (const std::string& t : p.tags)
        rx->add_feature(rx, t.c_str());
    return true;
}

bool init(const clap_preset_discovery_provider_t* p)
{
    Provider* self = Provider::from(p);
    const clap_preset_discovery_indexer_t* ix = self->indexer;
    if (!ix)
        return false;
    const clap_preset_discovery_filetype_t type = {"Substrike Preset", "A Substrike kick", "substrike"};
    if (!ix->declare_filetype(ix, &type))
        return false;
    const clap_preset_discovery_location_t factory = {CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT,
                                                      "Substrike Factory Presets", CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN,
                                                      nullptr};
    ix->declare_location(ix, &factory);
    // The user folder is only declared when it exists: the plugin never
    // creates folders behind the user's back.
    self->userDir = userPresetDir();
    std::error_code ec;
    if (std::filesystem::is_directory(toPath(self->userDir), ec))
    {
        const clap_preset_discovery_location_t user = {CLAP_PRESET_DISCOVERY_IS_USER_CONTENT,
                                                       "Substrike User Presets", CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                                       self->userDir.c_str()};
        ix->declare_location(ix, &user);
    }
    return true;
}

void destroy(const clap_preset_discovery_provider_t* p) { delete Provider::from(p); }

bool getMetadata(const clap_preset_discovery_provider_t*, uint32_t kind, const char* location,
                 const clap_preset_discovery_metadata_receiver_t* rx)
{
    if (!rx)
        return false;
    if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
    {
        for (const PresetInfo& p : factoryPresets())
            if (!describe(rx, p, p.key.c_str(), true))
                return true; // the receiver asked to stop
        return true;
    }
    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE || !location || !location[0])
    {
        rx->on_error(rx, 0, "unsupported preset location");
        return false;
    }
    StateDocument doc;
    if (!readPresetFile(location, doc))
    {
        rx->on_error(rx, 0, "not a Substrike preset");
        return false;
    }
    PresetInfo info = presetInfo(doc);
    if (info.name.empty())
        info.name = fromPath(toPath(location).stem());
    // One preset per file: no load key.
    describe(rx, info, nullptr, false);
    return true;
}

const void* getExtension(const clap_preset_discovery_provider_t*, const char*) { return nullptr; }

uint32_t count(const clap_preset_discovery_factory_t*) { return 1; }

const clap_preset_discovery_provider_descriptor_t* descriptor(const clap_preset_discovery_factory_t*, uint32_t i)
{
    return i == 0 ? &kDescriptor : nullptr;
}

const clap_preset_discovery_provider_t* create(const clap_preset_discovery_factory_t*,
                                               const clap_preset_discovery_indexer_t* indexer, const char* id)
{
    if (!indexer || !id || std::strcmp(id, kProviderId) != 0)
        return nullptr;
    auto* self = new Provider();
    self->indexer = indexer;
    self->iface.desc = &kDescriptor;
    self->iface.provider_data = self;
    self->iface.init = init;
    self->iface.destroy = destroy;
    self->iface.get_metadata = getMetadata;
    self->iface.get_extension = getExtension;
    return &self->iface;
}

const clap_preset_discovery_factory_t kFactory = {count, descriptor, create};

} // namespace

const clap_preset_discovery_factory_t* presetDiscoveryFactory() { return &kFactory; }

} // namespace substrike
