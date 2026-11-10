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
    } else if ((data.size() == 0x8000 && std::memcmp(&data[4], "\xc3\xc2\xcd" "80", 5) == 0) ||
               (data.size() == 0x8002 && std::memcmp(&data[6], "\xc3\xc2\xcd" "80", 5) == 0)) {
        // A raw 32K freezer image starting with CBM80 is an Action Replay
        // (four 8K banks), with or without a two-byte load address in front.
        const size_t skip = data.size() - 0x8000;
        for (int bank = 0; bank < 4; ++bank) {
            Chip c;
            c.bank = bank;
            c.load = 0x8000;
            c.data.assign(data.begin() + static_cast<long>(skip + bank * 0x2000),
                          data.begin() + static_cast<long>(skip + (bank + 1) * 0x2000));
            chips.push_back(std::move(c));
        }
        type = 1;
    } else if (data.size() == 0x2000 || data.size() == 0x4000 ||
               (data.size() > 9 && data.size() < 0x4000 && std::memcmp(&data[4], "\xc3\xc2\xcd" "80", 5) == 0)) {
        // A raw image: 8K at $8000, or 16K at $8000-$BFFF. A shorter one that
        // starts with the cartridge signature (CBM80) is a ROM image whose
        // padding was left off -- Cynthcart ships its cartridge like that --
        // and is padded up to the next chip with $FF, as an EPROM is.
        std::vector<uint8_t> padded = data;
        padded.resize(padded.size() <= 0x2000 ? 0x2000 : 0x4000, 0xff);
        const std::vector<uint8_t>& image = padded;
        for (size_t off = 0; off < image.size(); off += 0x2000) {
            Chip c;
            c.load = static_cast<uint16_t>(0x8000 + off);
            c.data.assign(image.begin() + static_cast<long>(off), image.begin() + static_cast<long>(off + 0x2000));
            chips.push_back(std::move(c));
        }
        exrom = true;
        game = image.size() == 0x4000;
    } else {
        error = "not a cartridge image (.crt, or a raw 8K, 16K or Action Replay .bin)";
        return false;
    }

    switch (type) {
    case 0: case 1: case 3: case 4: case 5: case 7: case 8: case 13: case 19: case 32:
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
    ram_.assign(type == 1 ? 0x2000 : 0, 0);
    reset();
    return true;
}

void Cartridge::eject()
{
    type_ = -1;
    chips_.clear();
    image_.clear();
    name_.clear();
    ram_.clear();
    s = CartState{};
    x = CartFreezerState{};
}

void Cartridge::reset()
{
    const auto ram = s;
    s = CartState{};
    std::memcpy(s.ram, ram.ram, sizeof(s.ram));
    x = CartFreezerState{};
    setLines(exromInit_, gameInit_);
    switch (type_) {
    case 32: // EasyFlash boots in Ultimax mode from bank 0
        setLines(false, true);
        break;
    case 1: // Action Replay: 8K mode, bank 0, ROM
        setLines(true, false);
        break;
    case 3:  // Final Cartridge III: 16K, bank 0
    case 13: // Final Cartridge I: 16K
        setLines(true, true);
        break;
    default:
        break;
    }
}

// The freeze button: the cartridge takes the bus in Ultimax mode from bank 0
// (its NMI vector at $FFFA) and pulls NMI. The program running is frozen,
// and the cartridge's menu comes up; leaving it restores the machine.
void Cartridge::freeze()
{
    if (!freezable())
        return;
    s.enabled = true;
    s.bank = 0;
    x.locked = false;
    setLines(false, true);
    if (type_ == 1)
        x.ramMapped = true; // it keeps what it saves of the machine there
    x.nmi = true;
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
    if (type_ == 1 && x.ramMapped && !x.locked)
        return ram_.data();
    return find(s.bank, 0x8000);
}

const uint8_t* Cartridge::romh() const
{
    if (!present() || !s.enabled)
        return nullptr;
    // The Action Replay has one ROM, selected by ROML and ROMH alike.
    if (type_ == 1)
        return find(s.bank, 0x8000);
    // Ocean 512K images put the upper banks at $8000 too, but ROMH is $A000.
    const uint8_t* p = find(s.bank, game() && !exrom() ? 0xe000 : 0xa000);
    return p;
}

bool Cartridge::peekIo(uint16_t addr, uint8_t& v) const
{
    if (!present())
        return false;
    const bool io1 = addr < 0xdf00;
    switch (type_) {
    case 32:
        if (io1)
            return false;
        v = s.ram[addr & 0xff];
        return true;
    case 1: { // Action Replay: I/O 2 is the last page of ROML (its ROM or RAM)
        if (io1 || x.locked)
            return false;
        const uint8_t* p = x.ramMapped ? ram_.data() : find(s.bank, 0x8000);
        if (!p)
            return false;
        v = p[0x1f00 + (addr & 0xff)];
        return true;
    }
    case 3:   // Final Cartridge III: I/O 1 and 2 are the last two pages of ROML
    case 13: { // Final Cartridge I, likewise
        const uint8_t* p = find(type_ == 3 ? s.bank : 0, 0x8000);
        if (!p)
            return false;
        v = p[(io1 ? 0x1e00 : 0x1f00) + (addr & 0xff)];
        return true;
    }
    default:
        return false;
    }
}

bool Cartridge::readIo(uint16_t addr, uint8_t& v, bool& changed)
{
    changed = false;
    if (!peekIo(addr, v))
        return false;
    if (type_ == 13) {
        // Final Cartridge I: any access to I/O 1 switches it off, to I/O 2 on.
        const bool on = addr >= 0xdf00;
        changed = s.exrom != on || s.game != on;
        setLines(on, on);
        x.nmi = false;
    }
    return true;
}

bool Cartridge::writeIo(uint16_t addr, uint8_t v)
{
    if (!present())
        return false;
    const bool io1 = addr < 0xdf00;
    switch (type_) {
    case 1: // Action Replay: the register at $DE00-$DEFF, RAM in I/O 2
        if (x.locked)
            return false;
        if (!io1) {
            if (x.ramMapped)
                ram_[0x1f00 + (addr & 0xff)] = v;
            return false;
        }
        x.reg = v;
        setLines((v & 0x02) == 0, (v & 0x01) != 0); // bit 1: EXROM high, bit 0: GAME low
        s.bank = (v >> 3) & 0x03;
        x.ramMapped = (v & 0x20) != 0;
        if (v & 0x40)
            x.nmi = false; // the freeze is over
        if (v & 0x04)
            x.locked = true; // off until the next reset
        return true;
    case 3: // Final Cartridge III: the register at $DFFF
        if (io1 || (addr & 0xff) != 0xff || x.locked)
            return false;
        x.reg = v;
        s.bank = v & 0x03;
        setLines((v & 0x10) == 0, (v & 0x20) == 0); // bit 4: EXROM, bit 5: GAME (both low = asserted)
        x.nmi = (v & 0x40) == 0;
        if (v & 0x80)
            x.locked = true; // the register hides until the next reset
        return true;
    case 13: { // Final Cartridge I: see readIo
        const bool on = !io1;
        const bool changed = s.exrom != on || s.game != on;
        setLines(on, on);
        x.nmi = false;
        return changed;
    }
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
