#pragma once

// The 1541 disk drive: its own 6502 at 1 MHz with 2K of RAM and the DOS ROM,
// VIA 1 on the serial bus ($1800) and VIA 2 on the disk mechanics ($1C00).
//
// The disk turns under the head as a ring of GCR bytes (Gcr.h), one byte every
// 26-32 cycles depending on the density the DOS selects. A byte that is not
// part of a sync is latched into VIA 2 port A, pulses CA1 and, with SOE on,
// sets the CPU's overflow flag through the SO pin -- the BVC * loops the DOS
// and fast loaders wait in. Ten or more one-bits in a row (here: two $FF bytes)
// are a sync, seen on PB7. In write mode (CB2 low) the byte in port A is
// written at the head instead. The stepper moves the head in half tracks.
//
// The serial bus: VIA 1 port B drives CLK and DATA and the ATN acknowledge;
// the machine combines every device's pulls into the lines (readBus) and
// tells the drive about the C64's outputs (setC64Lines, for the ATN
// interrupt on CA1 and to wake it from sleep).

#include <cstdint>
#include <functional>
#include <vector>

#include "Cpu6502.h"
#include "Gcr.h"
#include "Via6522.h"

namespace resident::emu {

struct DriveState
{
    uint64_t clk = 0;
    uint8_t ram[2048] = {};
    int halftrack = 36;      // track 18
    uint8_t stepPhase = 0;
    size_t headPos = 0;
    int byteTimer = 26;
    uint8_t lastByte = 0;
    uint8_t readLatch = 0;
    bool sync = false;
    bool c64Atn = false, c64Clk = false, c64Data = false; // asserted (line low)
    uint64_t lastBusChange = 0;
    bool sleeping = false;
};

class Drive1541
{
public:
    static constexpr double kClockHz = 1000000.0;

    Drive1541();
    void setRom(const std::vector<uint8_t>& rom);
    bool hasRom() const { return rom_.size() == 16384; }
    void reset();

    GcrDisk disk;
    bool writeProtect = false;
    int deviceNumber = 8;

    // The C64's outputs on the bus (true = it pulls the line low): for the
    // ATN interrupt and to wake a sleeping drive.
    void setC64Lines(bool atn, bool clk, bool data);
    // The lines as everybody drives them (true = low), from whoever wires the bus.
    std::function<void(bool& atn, bool& clk, bool& data)> readBus;
    // What the drive itself pulls. DATA includes the automatic answer to ATN:
    // the drive holds DATA low while ATN and its acknowledge (PB4) disagree.
    bool pullsClk() const { return driveClk(); }
    bool pullsData(bool atnLine) const { return driveData() || (atnLine != atnAck()); }

    // Runs the drive up to its own cycle `target`. A drive with its motor off
    // and a quiet bus for a few seconds sleeps (its clock follows without
    // executing) until the C64 touches the bus again.
    void runUntil(uint64_t target);
    uint64_t clk() const { return s.clk; }
    bool motorOn() const { return via2.portBOut() & 0x04; }
    bool ledOn() const { return via2.portBOut() & 0x08; }
    int track() const { return s.halftrack / 2; }

    // CPU bus.
    uint8_t read(uint16_t addr);
    void write(uint16_t addr, uint8_t v);
    bool irqLine() const { return via1.irq() || via2.irq(); }
    bool nmiLine() const { return false; }
    bool trap(uint16_t) { return false; }

    Cpu6502<Drive1541> cpu;
    Via6522 via1, via2;
    DriveState s;

private:
    bool driveClk() const { return (via1.s.orb & via1.s.ddrb & 0x08) != 0; }
    bool driveData() const { return (via1.s.orb & via1.s.ddrb & 0x02) != 0; }
    bool atnAck() const { return (via1.s.orb & via1.s.ddrb & 0x10) != 0; }
    void tick();
    void rotate();
    void stepper();
    const std::vector<uint8_t>& currentTrack() const;

    std::vector<uint8_t> rom_;
};

} // namespace resident::emu
