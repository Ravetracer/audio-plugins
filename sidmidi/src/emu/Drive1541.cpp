#include "Drive1541.h"

namespace sidmidi::emu {

namespace {

// A drive with its motor off and nobody on the bus for this long goes to sleep.
constexpr uint64_t kSleepAfter = 3000000;

const std::vector<uint8_t> kNoTrack;

} // namespace

Drive1541::Drive1541() : cpu(*this)
{
    via1.readPortB = [this] {
        // Inverted through the 74LS14s: a line pulled low reads 1. Bits 5-6
        // are the device number jumpers (open = device 8).
        bool atn = s.c64Atn, clk = s.c64Clk || driveClk(), data = s.c64Data || pullsData(s.c64Atn);
        if (readBus)
            readBus(atn, clk, data);
        uint8_t v = 0;
        if (data)
            v |= 0x01;
        if (clk)
            v |= 0x04;
        if (atn)
            v |= 0x80;
        v |= static_cast<uint8_t>(((deviceNumber - 8) & 3) << 5);
        // Output bits read back what is driven.
        return static_cast<uint8_t>(v | 0x1a);
    };
    via1.readPortA = [] { return uint8_t{0xff}; };
    via2.readPortA = [this] { return s.readLatch; };
    via2.readPortB = [this] {
        uint8_t v = 0x6f; // outputs read back through the DDR mask anyway
        if (!writeProtect)
            v |= 0x10;
        if (!s.sync)
            v |= 0x80;
        return v;
    };
    via2.outputsChanged = [this] { stepper(); };
}

void Drive1541::setRom(const std::vector<uint8_t>& rom)
{
    rom_ = rom;
}

void Drive1541::reset()
{
    via1.reset();
    via2.reset();
    s.stepPhase = 0;
    s.sync = false;
    s.sleeping = false;
    s.lastBusChange = s.clk;
    if (hasRom())
        cpu.reset();
}

void Drive1541::setC64Lines(bool atn, bool clk, bool data)
{
    if (atn != s.c64Atn || clk != s.c64Clk || data != s.c64Data) {
        s.lastBusChange = s.clk;
        s.sleeping = false;
    }
    s.c64Atn = atn;
    s.c64Clk = clk;
    s.c64Data = data;
    // ATN reaches CA1 of VIA 1 (inverted: asserted = high).
    via1.setCA1(atn);
}

const std::vector<uint8_t>& Drive1541::currentTrack() const
{
    if (!disk.present || (s.halftrack & 1))
        return kNoTrack;
    const int t = s.halftrack / 2;
    if (t < 1 || t > GcrDisk::kMaxTracks)
        return kNoTrack;
    return disk.tracks[static_cast<size_t>(t)];
}

void Drive1541::stepper()
{
    const uint8_t phase = via2.portBOut() & 0x03;
    if (phase == ((s.stepPhase + 1) & 3)) {
        if (s.halftrack < 2 * GcrDisk::kMaxTracks + 1)
            s.halftrack++;
    } else if (phase == ((s.stepPhase - 1) & 3)) {
        if (s.halftrack > 2)
            s.halftrack--;
    }
    s.stepPhase = phase;
}

void Drive1541::rotate()
{
    // Density 0-3 from PB5-6: a byte every 32, 30, 28 or 26 cycles.
    const int density = (via2.portBOut() >> 5) & 3;
    s.byteTimer = 32 - 2 * density;

    const auto& track = currentTrack();
    const bool writing = !via2.cb2High();
    if (track.empty()) {
        // No disk, or between tracks: noise without syncs.
        s.sync = false;
        s.lastByte = 0;
        return;
    }
    if (s.headPos >= track.size())
        s.headPos %= track.size();
    if (writing) {
        auto& t = disk.tracks[static_cast<size_t>(s.halftrack / 2)];
        if (!writeProtect) {
            t[s.headPos] = via2.portAOut();
            disk.dirty = true;
        }
        s.sync = false;
        s.lastByte = t[s.headPos];
        s.headPos = (s.headPos + 1) % t.size();
        via2.pulseCA1();
        if (via2.ca2High())
            cpu.s.p |= 0x40;
        return;
    }
    const uint8_t b = track[s.headPos];
    s.headPos = (s.headPos + 1) % track.size();
    s.sync = b == 0xff && s.lastByte == 0xff;
    s.lastByte = b;
    if (!s.sync) {
        s.readLatch = b;
        via2.pulseCA1();
        if (via2.ca2High())
            cpu.s.p |= 0x40; // BYTE READY on the SO pin
    }
}

void Drive1541::tick()
{
    via1.tick();
    via2.tick();
    if (motorOn() && --s.byteTimer <= 0)
        rotate();
    s.clk++;
}

void Drive1541::runUntil(uint64_t target)
{
    if (!hasRom()) {
        s.clk = target;
        return;
    }
    while (s.clk < target) {
        if (!s.sleeping && !motorOn() && !s.c64Atn && s.clk - s.lastBusChange > kSleepAfter)
            s.sleeping = true;
        if (s.sleeping) {
            s.clk = target;
            return;
        }
        cpu.step();
    }
}

uint8_t Drive1541::read(uint16_t addr)
{
    tick();
    if (addr < 0x1800)
        return s.ram[addr & 0x7ff];
    if (addr < 0x1c00)
        return via1.read(static_cast<uint8_t>(addr));
    if (addr < 0x2000)
        return via2.read(static_cast<uint8_t>(addr));
    if (addr >= 0x8000)
        return rom_[addr & 0x3fff];
    return static_cast<uint8_t>(addr >> 8); // open bus
}

void Drive1541::write(uint16_t addr, uint8_t v)
{
    tick();
    if (addr < 0x1800)
        s.ram[addr & 0x7ff] = v;
    else if (addr < 0x1c00)
        via1.write(static_cast<uint8_t>(addr), v);
    else if (addr < 0x2000)
        via2.write(static_cast<uint8_t>(addr), v);
}

} // namespace sidmidi::emu
