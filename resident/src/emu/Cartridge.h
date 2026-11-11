#pragma once

// The expansion port: a cartridge's ROM banks, its EXROM/GAME lines and its
// bank-switching registers in I/O 1 and I/O 2.
//
// Types: normal 8K/16K/Ultimax (CRT type 0, and raw .bin images of 8K or
// 16K), Ocean (5), Fun Play (7), Super Games (8), Magic Desk (19), EasyFlash
// (32, flash read-only with its 256 bytes of RAM at $DF00) and Simons' BASIC
// (4); and the freezer cartridges: Action Replay 4.x-7 (1, 32K ROM in four
// banks, 8K RAM, register at $DE00; also as a raw 32K .bin), Final Cartridge
// III (3, 64K in four 16K banks, register at $DFFF) and Final Cartridge I
// (13, 16K, switched off by I/O 1 and on by I/O 2). Their freeze button is
// freeze(). Anything else is refused with a message rather than run wrongly.

#include <cstdint>
#include <string>
#include <vector>

namespace resident::emu {

struct CartState
{
    int bank = 0;
    bool exrom = false; // asserted (line low)
    bool game = false;  // asserted (line low)
    bool enabled = true;
    uint8_t ram[256] = {}; // EasyFlash's $DF00 RAM
};

// The freezer cartridges' state, kept apart from CartState so snapshots from
// before they existed still read.
struct CartFreezerState
{
    uint8_t reg = 0;
    bool locked = false;    // AR: switched off until reset; FC3: register hidden
    bool ramMapped = false; // AR: its RAM at ROML ($8000) and I/O 2
    bool nmi = false;       // the cartridge holds NMI low
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
    // The freeze button (Action Replay, Final Cartridge I and III).
    bool freezable() const { return type_ == 1 || type_ == 3 || type_ == 13; }
    void freeze();
    bool nmiLine() const { return present() && x.nmi; }
    bool exrom() const { return present() && s.enabled && s.exrom; }
    bool game() const { return present() && s.enabled && s.game; }

    // ROM reads; null when nothing is mapped (Ultimax open bus).
    const uint8_t* roml() const;
    const uint8_t* romh() const;
    // Cartridge RAM a write to ROML lands in (Action Replay), or null.
    uint8_t* romlRam() { return type_ == 1 && x.ramMapped && !x.locked ? ram_.data() : nullptr; }

    // I/O 1 ($DE00-$DEFF) and I/O 2 ($DF00-$DFFF). Return false when the
    // cartridge does not answer the address. A write, and for some types a
    // read (`changed`), returns true when the memory configuration may have
    // changed. peekIo reads without side effects.
    bool readIo(uint16_t addr, uint8_t& v, bool& changed);
    bool peekIo(uint16_t addr, uint8_t& v) const;
    bool writeIo(uint16_t addr, uint8_t v);

    // The Action Replay's RAM, for the snapshot.
    std::vector<uint8_t>& ram() { return ram_; }
    const std::vector<uint8_t>& ram() const { return ram_; }

    CartState s;
    CartFreezerState x;

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
    std::vector<uint8_t> ram_; // the Action Replay's 8K
};

} // namespace resident::emu
