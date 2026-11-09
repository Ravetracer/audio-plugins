#pragma once

// The disk as the 1541's head sees it: per track a ring of GCR bytes, laid
// out the way a 1541 formats a disk -- for every sector a sync, the header
// block, a gap, a sync, the data block and an inter-sector gap, the track
// length set by its speed zone.
//
// fromD64() builds that from a .d64; toD64() reads every sector it can find
// back out of the GCR (what the drive wrote included) into a .d64.

#include <array>
#include <cstdint>
#include <vector>

#include "D64.h"

namespace sidmidi::emu {

struct GcrDisk
{
    static constexpr int kMaxTracks = 42;
    // Index = track number (1..42); empty when the track does not exist.
    std::array<std::vector<uint8_t>, kMaxTracks + 1> tracks;
    bool present = false;
    bool dirty = false;

    static int zoneOf(int track) { return track <= 17 ? 3 : track <= 24 ? 2 : track <= 30 ? 1 : 0; }
    static size_t trackBytes(int track)
    {
        static constexpr size_t sizes[4] = {6250, 6666, 7142, 7692};
        return sizes[zoneOf(track)];
    }

    void fromD64(const D64& d64);
    // Decodes every sector found into `d64` (which must hold an image of the
    // same geometry, e.g. the one fromD64 was built from). Returns the number
    // of sectors that decoded.
    int toD64(D64& d64) const;
    void clear()
    {
        for (auto& t : tracks)
            t.clear();
        present = false;
        dirty = false;
    }
};

} // namespace sidmidi::emu
