#pragma once

#include <clap/clap.h>

namespace shapeau {

// The preset discovery factory, built on first use. See preset_provider.cpp.
const clap_preset_discovery_factory_t *presetDiscoveryFactory();

} // namespace shapeau
