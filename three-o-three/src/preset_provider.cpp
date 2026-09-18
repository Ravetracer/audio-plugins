// Preset discovery for ThreeOhThree. The provider itself is shared by the
// suite; this only says which plugin it is serving.

#include "factories.h"
#include "params.h"
#include "presets_generated.h"
#include "threeohthree.h"

#include "plugincore/preset_provider.h"

namespace threeohthree {

namespace {

const PresetProviderSpec kSpec = {
   {kPluginName, kPresetExtension, paramTable(), kNumParams},
   kProviderId,
   kPluginId,
   kPluginVendor,
   "ThreeOhThree bass-line preset",
   kBuiltinPresets,
   kNumBuiltinPresets,
};

} // namespace

// Built on first use rather than at static-initialisation time, so the
// parameter table it points at is certainly alive by then.
const clap_preset_discovery_factory_t *presetDiscoveryFactory() {
   static const clap_preset_discovery_factory_t *f = plugincore::presetDiscoveryFactory(kSpec);
   return f;
}

} // namespace threeohthree
