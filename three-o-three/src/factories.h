#pragma once

#include <clap/clap.h>

namespace threeohthree {

// Implemented in plugin.cpp / preset_provider.cpp and handed out by the
// plugin entry point.
extern const clap_plugin_factory_t gPluginFactory;
const clap_preset_discovery_factory_t *presetDiscoveryFactory();

} // namespace threeohthree
