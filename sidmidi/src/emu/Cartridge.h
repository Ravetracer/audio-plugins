#pragma once

// The expansion port: a cartridge's ROM banks, its EXROM/GAME lines and its
// bank-switching registers in I/O 1 and I/O 2.
//
// Types: normal 8K/16K/Ultimax (CRT type 0, and raw .bin images of 8K or
// 16K), Ocean (5), Fun Play (7), Super Games (8), Magic Desk (19), EasyFlash
// (32, flash read-only with its 256 bytes of RAM at $DF00) and Simons' BASIC
// (4). Anything else is refused with a message rather than run wrongly.

#include <cstdint>
#include <string>
#include <vector>

namespace sidmidi::emu {

struct CartState
{
    int bank = 0;
    bool exrom = false; // asserted (line low)
    bool game = false;  // asserted (line low)
    bool enabled = true;
    uint8_t ram[256] = {}; // EasyFlash's $DF00 RAM
};

class Cartridge
{
public:
    // Loads a .crt or a raw image. Returns false with a reason.
    bool load(const std::vector<uint8_t>& data, const std::string& name, std::string& error);
    void eject();
    bool present() const { return type_ >= 0; }
    int crtType() const { return type_; }
    const std::string& name() const { return name_; }
    const std::vector<uint8_t>& image() const { return image_; }

    void reset();
    bool exrom() const { return present() && s.enabled && s.exrom; }
    bool game() const { return present() && s.enabled && s.game; }

    // ROM reads; null when nothing is mapped (Ultimax open bus).
    const uint8_t* roml() const;
    const uint8_t* romh() const;

    // I/O 1 ($DE00-$DEFF) and I/O 2 ($DF00-$DFFF). Return false when the
    // cartridge does not answer the address. A write returns true when the
    // memory configuration may have changed.
    bool readIo(uint16_t addr, uint8_t& v) const;
    bool writeIo(uint16_t addr, uint8_t v);

    CartState s;

private:
    struct Chip
    {
        int bank = 0;
        uint16_t load = 0x8000;
        std::vector<uint8_t> data; // 8K
    };
    const uint8_t* find(int bank, uint16_t base) const;
    void setLines(bool exrom, bool game)
    {
        s.exrom = exrom;
        s.game = game;
    }

    int type_ = -1;
    bool exromInit_ = false, gameInit_ = false;
    std::string name_;
    std::vector<uint8_t> image_;
    std::vector<Chip> chips_;
};

} // namespace sidmidi::emu
