// Preset discovery: tells the host where a plugin's presets live and what is in
// them, so they show up in the host's own browser (Bitwig's Sound Content
// browser, for example) without the plugin needing a GUI of its own.
//
// Shared by the whole suite. A plugin supplies a PresetProviderSpec and gets a
// clap_preset_discovery_factory_t back. Because shared/ is a static library
// linked into each plugin separately, the file-scope state below is private to
// one plugin binary.

#include <cstring>
#include <string>

#include <filesystem>

#include <clap/clap.h>

#include "plugincore/preset_provider.h"

namespace plugincore {

namespace {

// Set once by presetDiscoveryFactory(), before the host can call anything.
const PresetProviderSpec *gSpec = nullptr;

// The descriptor and the display strings are built from the spec on first use
// and must outlive every call the host makes, so they are held here.
struct Strings {
   std::string providerName;
   std::string filetypeName;
   std::string factoryLocation;
   std::string userLocation;
};
Strings &strings() {
   static Strings s;
   return s;
}

clap_preset_discovery_provider_descriptor_t &providerDescriptor() {
   static clap_preset_discovery_provider_descriptor_t d{};
   return d;
}

clap_universal_plugin_id_t &universalId() {
   static clap_universal_plugin_id_t id{};
   return id;
}

struct Provider {
   clap_preset_discovery_provider_t iface{};
   const clap_preset_discovery_indexer_t *indexer = nullptr;
   std::string factoryDir;
   std::string userDir;
   bool useBundled = false;

   static Provider *from(const clap_preset_discovery_provider_t *p) {
      return static_cast<Provider *>(p->provider_data);
   }
};

// Reports one preset's metadata to the indexer.
bool describePreset(const clap_preset_discovery_metadata_receiver_t *rx, const PresetData &preset,
                    const char *loadKey, bool factoryContent) {
   if (!rx->begin_preset(rx, preset.name.c_str(), loadKey ? loadKey : ""))
      return false;

   rx->add_plugin_id(rx, &universalId());
   rx->set_flags(rx, factoryContent ? CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT
                                    : CLAP_PRESET_DISCOVERY_IS_USER_CONTENT);
   if (!preset.author.empty())
      rx->add_creator(rx, preset.author.c_str());
   if (!preset.description.empty())
      rx->set_description(rx, preset.description.c_str());

   // Plugin category first so the host knows these presets produce an
   // instrument, then the preset's own descriptive tags.
   rx->add_feature(rx, CLAP_PLUGIN_FEATURE_INSTRUMENT);
   rx->add_feature(rx, CLAP_PLUGIN_FEATURE_SYNTHESIZER);
   for (const auto &f : preset.features)
      rx->add_feature(rx, f.c_str());

   return true;
}

bool providerInit(const clap_preset_discovery_provider_t *p) {
   Provider *self = Provider::from(p);
   const clap_preset_discovery_indexer_t *ix = self->indexer;
   if (!ix)
      return false;

   const clap_preset_discovery_filetype_t filetype = {
      strings().filetypeName.c_str(), gSpec->filetypeDescription,
      gSpec->preset.presetExtension};
   if (!ix->declare_filetype(ix, &filetype))
      return false;

   self->factoryDir = factoryPresetDir(gSpec->preset);
   self->userDir = userPresetDir(gSpec->preset);

   if (!self->factoryDir.empty()) {
      const clap_preset_discovery_location_t location = {
         CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, strings().factoryLocation.c_str(),
         CLAP_PRESET_DISCOVERY_LOCATION_FILE, self->factoryDir.c_str()};
      ix->declare_location(ix, &location);
   } else {
      // No preset directory next to the binary (single-file install): fall back
      // to the copies compiled into the plugin itself, with the plugin acting
      // as its own preset container.
      self->useBundled = true;
      const clap_preset_discovery_location_t location = {
         CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, strings().factoryLocation.c_str(),
         CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr};
      ix->declare_location(ix, &location);
   }

   // The user location is only declared when it already exists, so the plugin
   // never creates directories behind the user's back.
   if (!self->userDir.empty()) {
      std::error_code ec;
      if (std::filesystem::is_directory(self->userDir, ec)) {
         const clap_preset_discovery_location_t location = {
            CLAP_PRESET_DISCOVERY_IS_USER_CONTENT, strings().userLocation.c_str(),
            CLAP_PRESET_DISCOVERY_LOCATION_FILE, self->userDir.c_str()};
         ix->declare_location(ix, &location);
      }
   }

   return true;
}

void providerDestroy(const clap_preset_discovery_provider_t *p) { delete Provider::from(p); }

bool providerGetMetadata(const clap_preset_discovery_provider_t *p, uint32_t locationKind,
                         const char *location,
                         const clap_preset_discovery_metadata_receiver_t *rx) {
   Provider *self = Provider::from(p);
   if (!rx)
      return false;

   if (locationKind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
      // The plugin is the container: enumerate every built-in preset.
      for (unsigned i = 0; i < gSpec->builtinCount; ++i) {
         PresetData preset;
         std::string error;
         if (!parsePreset(gSpec->preset, gSpec->builtins[i].text,
                          std::strlen(gSpec->builtins[i].text), preset,
                          error))
            continue;
         if (preset.name.empty())
            preset.name = gSpec->builtins[i].loadKey;
         if (!describePreset(rx, preset, gSpec->builtins[i].loadKey, true))
            return true; // receiver asked us to stop
      }
      return true;
   }

   if (locationKind != CLAP_PRESET_DISCOVERY_LOCATION_FILE || !location || !location[0]) {
      rx->on_error(rx, 0, "unsupported preset location");
      return false;
   }

   PresetData preset;
   std::string error;
   if (!parsePresetFile(gSpec->preset, location, preset, error)) {
      rx->on_error(rx, 0, error.c_str());
      return false;
   }

   const bool isUser =
      !self->userDir.empty() && std::strncmp(location, self->userDir.c_str(), self->userDir.size()) == 0;
   // A single-preset file needs no load key.
   describePreset(rx, preset, nullptr, !isUser);
   return true;
}

const void *providerGetExtension(const clap_preset_discovery_provider_t *, const char *) {
   return nullptr;
}

uint32_t factoryCount(const clap_preset_discovery_factory_t *) { return 1; }

const clap_preset_discovery_provider_descriptor_t *
factoryGetDescriptor(const clap_preset_discovery_factory_t *, uint32_t index) {
   return index == 0 ? &providerDescriptor() : nullptr;
}

const clap_preset_discovery_provider_t *
factoryCreate(const clap_preset_discovery_factory_t *,
              const clap_preset_discovery_indexer_t *indexer, const char *providerId) {
   if (!indexer || !providerId || std::strcmp(providerId, gSpec->providerId) != 0)
      return nullptr;

   auto *self = new Provider();
   self->indexer = indexer;
   self->iface.desc = &providerDescriptor();
   self->iface.provider_data = self;
   self->iface.init = providerInit;
   self->iface.destroy = providerDestroy;
   self->iface.get_metadata = providerGetMetadata;
   self->iface.get_extension = providerGetExtension;
   return &self->iface;
}

const clap_preset_discovery_factory_t kFactory = {factoryCount, factoryGetDescriptor,
                                                  factoryCreate};

} // namespace

const clap_preset_discovery_factory_t *presetDiscoveryFactory(const PresetProviderSpec &spec) {
   gSpec = &spec;

   const std::string name = spec.preset.pluginName;
   Strings &s = strings();
   s.providerName = name + " Presets";
   s.filetypeName = name + " Preset";
   s.factoryLocation = name + " Factory Presets";
   s.userLocation = name + " User Presets";

   providerDescriptor() = {CLAP_VERSION_INIT, spec.providerId, s.providerName.c_str(),
                           spec.vendor};
   universalId() = {"clap", spec.pluginId};

   return &kFactory;
}

} // namespace plugincore
