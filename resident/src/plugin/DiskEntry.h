#pragma once

#include <string>

namespace resident {

// A file on the inserted disk, as the window lists it.
struct DiskEntry
{
    std::string petscii;  // the name as on the disk, for LOAD
    std::string display;  // the same, readable
    std::string type;     // PRG, SEQ, USR, REL, DEL
    int blocks = 0;
};

} // namespace resident
