#pragma once

#include <cstring>
#include <string>

#include <filesystem>

#include <clap/clap.h>

#include "plugincore/preset.h"

// CLAP preset discovery, shared by the suite. A plugin describes itself once
// and gets back a factory it can hand to the host.

namespace plugincore {

struct PresetProviderSpec {
   PresetContext preset;
   const char *providerId; // e.g. "de.ravetracer.rainyday.preset-provider"
   const char *pluginId;   // the plugin's own CLAP id
   const char *vendor;
   // Shown by the host beside the file type, e.g. "RainyDay rain synthesis
   // preset". The other display strings are derived from the plugin name.
   const char *filetypeDescription;
   const BuiltinPreset *builtins;
   unsigned builtinCount;
};

// Binds `spec` to the shared provider and returns the factory. `spec` must
// outlive the factory, so pass something with static storage duration. Call
// once per plugin: shared/ is a static library linked into each plugin
// separately, so the binding is private to one plugin binary.
const clap_preset_discovery_factory_t *presetDiscoveryFactory(const PresetProviderSpec &spec);

} // namespace plugincore
