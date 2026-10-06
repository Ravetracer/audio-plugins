// Exported plugin entry, compiled once per plugin format by clap-wrapper.
#include <clap/clap.h>

#include "plugin/AurumPlugin.h"

namespace {
bool init(const char* path) { return aurum::entryInit(path); }
void deinit() { aurum::entryDeinit(); }
const void* getFactory(const char* id) { return aurum::entryGetFactory(id); }
} // namespace

extern "C" {
// clap/entry.h already declares this with CLAP_EXPORT, so the definition must
// not repeat the attribute: mingw rejects a second dllexport on a const object
// it then sees as having internal linkage.
const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, init, deinit, getFactory};
}
