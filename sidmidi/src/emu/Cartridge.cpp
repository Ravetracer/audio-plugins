#include "Cartridge.h"

#include <cstring>

namespace sidmidi::emu {

namespace {

uint32_t be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t be16(const uint8_t* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

} // namespace

bool Cartridge::load(const std::vector<uint8_t>& data, const std::string& name, std::string& error)
{
    std::vector<Chip> chips;
    int type = 0;
    bool exrom = true, game = false;

    if (data.size() >= 0x40 && std::memcmp(data.data(), "C64 CARTRIDGE   ", 16) == 0) {
        const uint32_t hdrLen = be32(&data[0x10]);
        type = be16(&data[0x16]);
        // The header stores the line levels: 0 = asserted (low).
        exrom = data[0x18] == 0;
        game = data[0x19] == 0;
        size_t at = hdrLen;
        while (at + 0x10 <= data.size()) {
            if (std::memcmp(&data[at], "CHIP", 4) != 0)
                break;
            const uint32_t len = be32(&data[at + 4]);
            const uint16_t bank = be16(&data[at + 0x0a]);
            const uint16_t load = be16(&data[at + 0x0c]);
            const uint16_t size = be16(&data[at + 0x0e]);
            if (at + 0x10 + size > data.size() || len < 0x10)
                break;
            // A 16K chip is split into ROML and ROMH halves.
            for (uint32_t off = 0; off < size; off += 0x2000) {
                Chip c;
                c.bank = bank;
                c.load = static_cast<uint16_t>(load + off);
                const uint32_t n = std::min<uint32_t>(0x2000, size - off);
                c.data.assign(0x2000, 0xff);
                std::memcpy(c.data.data(), &data[at + 0x10 + off], n);
                // An 8K Ultimax image at $E000, or 4K images, mirrored into place.
                if (n == 0x1000)
                    std::memcpy(c.data.data() + 0x1000, &data[at + 0x10 + off], 0x1000);
                chips.push_back(std::move(c));
            }
            at += len;
        }
        if (chips.empty()) {
            error = "the cartridge image holds no ROM";
            return false;
        }
    } else if (data.size() == 0x2000 || data.size() == 0x4000) {
        // A raw image: 8K at $8000, or 16K at $8000-$BFFF.
        for (size_t off = 0; off < data.size(); off += 0x2000) {
            Chip c;
            c.load = static_cast<uint16_t>(0x8000 + off);
            c.data.assign(data.begin() + static_cast<long>(off), data.begin() + static_cast<long>(off + 0x2000));
            chips.push_back(std::move(c));
        }
        exrom = true;
        game = data.size() == 0x4000;
    } else {
        error = "not a cartridge image (.crt, or a raw 8K/16K .bin)";
        return false;
    }

    switch (type) {
    case 0: case 4: case 5: case 7: case 8: case 19: case 32:
        break;
    default:
        error = "cartridge type " + std::to_string(type) + " is not supported yet";
        return false;
    }

    // The banked types start in the mode their header gives (Ocean 8K or 16K,
    // Magic Desk 8K), so the header lines are taken as they are.
    type_ = type;
    name_ = name;
    image_ = data;
    chips_ = std::move(chips);
    exromInit_ = exrom;
    gameInit_ = game;
    reset();
    return true;
}

void Cartridge::eject()
{
    type_ = -1;
    chips_.clear();
    image_.clear();
    name_.clear();
    s = CartState{};
}

void Cartridge::reset()
{
    const auto ram = s;
    s = CartState{};
    std::memcpy(s.ram, ram.ram, sizeof(s.ram));
    setLines(exromInit_, gameInit_);
    if (type_ == 32) // EasyFlash boots in Ultimax mode from bank 0
        setLines(false, true);
}

const uint8_t* Cartridge::find(int bank, uint16_t base) const
{
    for (const auto& c : chips_)
        if (c.bank == bank && c.load == base)
            return c.data.data();
    // Ultimax ROMH may be stored at $E000 or $A000.
    if (base == 0xa000) {
        for (const auto& c : chips_)
            if (c.bank == bank && c.load == 0xe000)
                return c.data.data();
    } else if (base == 0xe000) {
        for (const auto& c : chips_)
            if (c.bank == bank && c.load == 0xa000)
                return c.data.data();
    }
    return nullptr;
}

const uint8_t* Cartridge::roml() const
{
    if (!present() || !s.enabled)
        return nullptr;
    return find(s.bank, 0x8000);
}

const uint8_t* Cartridge::romh() const
{
    if (!present() || !s.enabled)
        return nullptr;
    // Ocean 512K images put the upper banks at $8000 too, but ROMH is $A000.
    const uint8_t* p = find(s.bank, game() && !exrom() ? 0xe000 : 0xa000);
    return p;
}

bool Cartridge::readIo(uint16_t addr, uint8_t& v) const
{
    if (!present())
        return false;
    if (type_ == 32 && addr >= 0xdf00) {
        v = s.ram[addr & 0xff];
        return true;
    }
    return false;
}

bool Cartridge::writeIo(uint16_t addr, uint8_t v)
{
    if (!present())
        return false;
    const bool io1 = addr < 0xdf00;
    switch (type_) {
    case 4: // Simons' BASIC: write to I/O 1 enables 16K, read disables (approximated by writes)
        if (io1) {
            setLines(true, true);
            return true;
        }
        return false;
    case 5: // Ocean: bank in the low 6 bits of $DE00
        if (io1) {
            s.bank = v & 0x3f;
            return true;
        }
        return false;
    case 7: // Fun Play / Power Play
        if (io1) {
            s.bank = ((v >> 3) & 0x07) | ((v & 0x01) << 3);
            s.enabled = (v & 0xc6) != 0x86;
            return true;
        }
        return false;
    case 8: // Super Games: $DF00, bank in bits 0-1, bit 2 disables
        if (!io1) {
            s.bank = v & 0x03;
            s.enabled = !(v & 0x04);
            return true;
        }
        return false;
    case 19: // Magic Desk: $DE00, bit 7 disables
        if (io1) {
            s.bank = v & 0x3f;
            s.enabled = !(v & 0x80);
            return true;
        }
        return false;
    case 32: // EasyFlash
        if (!io1) {
            s.ram[addr & 0xff] = v;
            return false;
        }
        if ((addr & 0x02) == 0) { // $DE00: bank
            s.bank = v & 0x3f;
        } else { // $DE02: control
            const bool gameFromReg = (v & 0x04) ? (v & 0x01) : true;
            setLines((v & 0x02) != 0, gameFromReg);
        }
        return true;
    default:
        return false;
    }
}

} // namespace sidmidi::emu
