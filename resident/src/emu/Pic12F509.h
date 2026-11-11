#pragma once

// The Microchip PIC12F509: the baseline 12-bit core (33 instructions, one
// instruction cycle = 1 us at its 4 MHz internal oscillator, two for anything
// that changes the program counter), 1024 words of program memory, the
// banked register file, the two-level stack, TMR0 and the watchdog. Enough to
// run real firmware -- here HerMIDI's.
//
// GPIO: what a pin reads is the external level for an input (TRIS bit 1) and
// the output latch for an output. Open-drain outputs are emulated the way the
// firmware does it (latch 0, TRIS switching between output and input).

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace resident::emu {

struct PicState
{
    uint16_t pc = 0;
    uint8_t w = 0, status = 0x18, fsr = 0, option = 0xff, tris = 0x3f, tmr0 = 0, latch = 0, osccal = 0;
    std::array<uint8_t, 64> ram{};
    uint16_t stack[2] = {0, 0};
    uint32_t wdt = 0;
    uint64_t clk = 0;
};

class Pic12F509
{
public:
    // External pin levels, bits 0-5.
    std::function<uint8_t()> readPins = [] { return uint8_t{0x3f}; };

    PicState s;

    // Parses an Intel HEX image (byte addresses, little-endian words).
    bool loadHex(const std::string& hex);
    bool hasProgram() const { return loaded_; }
    void reset();
    // One instruction; returns its cycles.
    int step();

    // A pin driven low by the chip (output, latch 0).
    bool drivesLow(int pin) const { return !(s.tris & (1 << pin)) && !(s.latch & (1 << pin)); }

private:
    uint8_t gpio() const;
    int addr(uint8_t f) const;
    uint8_t rd(uint8_t f) const;
    void wr(uint8_t f, uint8_t v);
    void setZ(bool z) { s.status = static_cast<uint8_t>(z ? (s.status | 0x04) : (s.status & ~0x04)); }
    void setC(bool c) { s.status = static_cast<uint8_t>(c ? (s.status | 0x01) : (s.status & ~0x01)); }
    void setDC(bool c) { s.status = static_cast<uint8_t>(c ? (s.status | 0x02) : (s.status & ~0x02)); }

    std::array<uint16_t, 1024> rom_{};
    bool loaded_ = false;
    bool pcWritten_ = false;
};

} // namespace resident::emu
