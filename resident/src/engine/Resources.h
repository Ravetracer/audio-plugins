#pragma once

// The ROMs and the palettes.
//
// The Commodore ROMs are not part of ReSIDent: they are copyrighted, and the
// user puts them into the ROM folder (romFolder()), which ReSIDent creates on
// first start together with a README saying what belongs there. Files are
// recognised by their content, so VICE's names, short names or any other
// name all work.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "emu/C64.h"

namespace resident {

// ReSIDent's own folder: $XDG_DATA_HOME/ReSIDent (~/.local/share/ReSIDent) on
// Linux, %APPDATA%\ReSIDent on Windows. The first call that finds it missing
// copies the old SidMidi folder's contents into it, if there is one.
std::string dataFolder();

// Where the ROMs go: $XDG_DATA_HOME/ReSIDent/ROMs (~/.local/share/ReSIDent/ROMs)
// on Linux, %APPDATA%\ReSIDent\ROMs on Windows. RESIDENT_ROM_DIR overrides it.
std::string romFolder();
// Creates the folder and its README.txt when they are missing.
void prepareRomFolder();

// Fills what it finds in `folder` (and its C64/ and DRIVES/ subfolders).
// Returns true when KERNAL, BASIC and the character ROM were found; `report`
// lists what is missing.
bool romsFromFolder(const std::string& folder, emu::Roms& out, std::string& report);

// The text the window and the README show.
std::string romInstructions();

struct Palette
{
    std::string key;   // file name without .vpl
    std::string name;  // the NAME: line
    std::array<uint32_t, 16> rgb{}; // 0xRRGGBB
};

const std::vector<Palette>& palettes();
const Palette& palette(const std::string& key);
bool parseVpl(const std::string& text, Palette& out);

// UTF-8 paths on every platform.
bool readFile(const std::string& path, std::vector<uint8_t>& out);
bool writeFile(const std::string& path, const std::vector<uint8_t>& data);
// Opens a folder in the desktop's file manager.
void openFolder(const std::string& path);
// Creates a folder (and its parents). True when it exists afterwards.
bool makeFolder(const std::string& path);
// The files in `folder` whose extension is `ext` (".ccmap"), any case, sorted.
std::vector<std::string> listFiles(const std::string& folder, const std::string& ext);
// The file name without folder and extension.
std::string fileStem(const std::string& path);
// `folder` + separator + `name`.
std::string joinPath(const std::string& folder, const std::string& name);

} // namespace resident
