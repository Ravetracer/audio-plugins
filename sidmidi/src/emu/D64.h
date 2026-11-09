#pragma once

// A .d64 disk image in memory: 35 or 40 tracks, the BAM and the directory on
// track 18, files as chains of 254-byte blocks. Enough of CBM DOS 2.6's
// filing system to read, write, scratch and rename files the way a 1541
// would, so what a program saves can be loaded again by a real drive.

#include <cstdint>
#include <string>
#include <vector>

namespace sidmidi::emu {

struct DirEntry
{
    std::string name;     // PETSCII, without the $A0 padding
    uint8_t type = 0;     // the raw type byte (0x82 = closed PRG)
    uint8_t track = 0, sector = 0;
    uint16_t blocks = 0;
    int slotTrack = 0, slotSector = 0, slotOffset = 0; // where the entry lives
};

class D64
{
public:
    static constexpr size_t kSize35 = 174848;
    static constexpr size_t kSize40 = 196608;

    bool load(const std::vector<uint8_t>& data, std::string& error);
    // A freshly formatted disk.
    void format(const std::string& name, const std::string& id);
    bool present() const { return !data_.empty(); }
    const std::vector<uint8_t>& data() const { return data_; }
    void eject() { data_.clear(); }

    int tracks() const { return tracks_; }
    static int sectorsIn(int track);
    uint8_t* sector(int track, int sec);
    const uint8_t* sector(int track, int sec) const;

    std::string diskName() const;
    std::string diskId() const;
    std::vector<DirEntry> directory() const;
    int blocksFree() const;

    // Finds the first entry matching a pattern (* and ? wildcards). `typeWanted`
    // 0 accepts any type.
    bool find(const std::string& pattern, DirEntry& out, uint8_t typeWanted = 0) const;
    std::vector<uint8_t> readFile(const DirEntry& e) const;
    // Writes a new file. Returns a DOS error code (0 = OK, 63 exists, 72 full).
    int writeFile(const std::string& name, uint8_t type, const std::vector<uint8_t>& bytes, bool replace);
    // Returns the number of files scratched.
    int scratch(const std::string& pattern);
    int rename(const std::string& from, const std::string& to);

    static bool matches(const std::string& pattern, const std::string& name);

private:
    size_t offset(int track, int sec) const;
    bool allocate(int& track, int& sec, int nearTrack, int nearSector);
    void freeBlock(int track, int sec);
    bool isFree(int track, int sec) const;
    void setBam(int track, int sec, bool free);
    bool freeDirSlot(int& t, int& s, int& off);

    std::vector<uint8_t> data_;
    int tracks_ = 35;
};

} // namespace sidmidi::emu
