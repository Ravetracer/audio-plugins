#pragma once

// Drive 8 as the KERNAL sees it, without a drive.
//
// The machine traps the KERNAL's serial bus routines (TALK, LISTEN, SECOND,
// TKSA, CIOUT, ACPTR, UNTLK, UNLSN) and calls the methods here instead; this
// class answers them the way CBM DOS 2.6 would, from the attached .d64. So
// LOAD, SAVE, OPEN/CHRIN/CHROUT on files, LOAD"$" and the command channel all
// work, instantly, for any program that goes through the KERNAL. Programs
// that talk to the drive themselves (fast loaders, M-E) need the true 1541.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "D64.h"

namespace resident::emu {

class VirtualDrive
{
public:
    // KERNAL status bits, as stored in $90.
    enum : uint8_t { ST_TIMEOUT_READ = 0x02, ST_EOI = 0x40, ST_DEVICE_NOT_PRESENT = 0x80 };

    int device = 8;
    D64 disk;
    bool dirty = false;      // the image changed since it was attached
    bool writeProtect = false;

    void reset();
    // Each returns the bits to OR into ST.
    uint8_t listen(uint8_t dev);
    uint8_t talk(uint8_t dev);
    uint8_t second(uint8_t sa);  // after LISTEN
    uint8_t tksa(uint8_t sa);    // after TALK
    uint8_t ciout(uint8_t byte);
    uint8_t acptr(uint8_t& byte);
    uint8_t unlisten();
    uint8_t untalk();

    bool busy() const { return activity_ > 0; }
    void tickActivity() { if (activity_) --activity_; }
    std::string status() const { return status_; }

    // Everything is in plain members so the snapshot can store it.
    struct Channel
    {
        bool open = false;
        bool writing = false;
        bool command = false;
        std::string name;
        uint8_t type = 2;
        bool replace = false;
        std::vector<uint8_t> data;
        size_t pos = 0;
    };
    std::array<Channel, 16> channels;
    bool listening = false, talking = false;
    bool opening = false;
    int channel = 0;
    std::string openName;
    std::string status_ = "73,CBM DOS V2.6 1541,00,00";
    size_t statusPos = 0;
    std::string command_;

private:
    void doOpen(int ch, const std::string& rawName);
    void doClose(int ch);
    void doCommand(const std::string& cmd);
    void setStatus(int code, const char* text, int t = 0, int s = 0);
    std::vector<uint8_t> directoryListing(const std::string& pattern) const;

    int activity_ = 0;
};

} // namespace resident::emu
