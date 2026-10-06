// Exported plugin entry, compiled once per plugin format by clap-wrapper.
#include <clap/clap.h>

#include "plugin/AurumPlugin.h"

namespace {
bool init(const char* path) { return aurum::entryInit(path); }
void deinit() { aurum::entryDeinit(); }
const void* getFactory(const char* id) { return aurum::entryGetFactory(id); }
} // namespace

extern "C" {
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, init, deinit, getFactory};
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}
