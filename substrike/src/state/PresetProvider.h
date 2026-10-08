#pragma once

#include <clap/clap.h>

namespace substrike {

// CLAP preset discovery: tells the host where Substrike's presets are and
// what is in them, so they show up in the host's own browser. The factory
// presets are inside the plugin (location PLUGIN, load key = file name); the
// user's folder is declared when it exists.
const clap_preset_discovery_factory_t* presetDiscoveryFactory();

} // namespace substrike
