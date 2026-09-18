// The CLAP module's entry point: the exported clap_entry, built from the entry
// functions the plugin's static library provides. See entry.h.

#include "entry.h"

extern "C" {

// clap/entry.h already declares this with CLAP_EXPORT, so the definition must
// not repeat the visibility attribute. The linker version script pins the
// export as well, making clap_entry the only symbol this DSO exposes.
const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, saeurekisteEntryInit,
                                        saeurekisteEntryDeinit, saeurekisteEntryGetFactory};
}
