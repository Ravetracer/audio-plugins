// The CLAP entry point, split in two so that more than one plugin format can be
// built from one implementation.
//
// Everything Shapeau is lives in a static library, which exports the three
// functions below instead of a clap_plugin_entry. Each format's module then
// compiles the one small translation unit that builds the entry structure from
// them: entry.cpp for the .clap.

#pragma once

namespace shapeau {

bool entryInit(const char *pluginPath);
void entryDeinit();
const void *entryGetFactory(const char *factoryId);

} // namespace shapeau
