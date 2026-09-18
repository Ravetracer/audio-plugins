// The CLAP entry point, split in two so that more than one plugin format can be
// built from one implementation.
//
// Everything ThreeOhThree is lives in a static library, which exports the three
// functions below instead of a clap_plugin_entry. Each format's module then
// compiles the one small translation unit that builds the entry structure from
// them: entry.cpp for the .clap. Only CLAP is built here, but the split costs
// nothing and is what the rest of the suite does.

#pragma once

#include <clap/clap.h>

extern "C" {

bool threeohthreeEntryInit(const char *pluginPath);
void threeohthreeEntryDeinit();
const void *threeohthreeEntryGetFactory(const char *factoryId);
}
