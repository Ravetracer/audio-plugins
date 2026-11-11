#pragma once

// MOS 6526 CIA: two 16-bit timers, the time-of-day clock, the serial shift
// register, two 8-bit ports and the interrupt control register.
//
// The timers follow the chip's pipeline: a timer starts counting two cycles
// after START is written, a forced load lands one cycle after the write, and
// on the old 6526 the IRQ line goes low one cycle after the interrupt flag is
// set (the 6526A raises it in the same cycle). A timer with latch N underflows
// every N+1 cycles.
//
// The ports are read through callbacks, because what is on a pin depends on
// the outside world: the keyboard matrix, the joysticks, the serial bus.

#include <cstdint>
#include <functional>

namespace resident::emu {

struct CiaState
{
    uint8_t pra = 0, prb = 0, ddra = 0, ddrb = 0;
    uint16_t ta = 0xffff, tb = 0xffff, latchA = 0xffff, latchB = 0xffff;
    uint8_t cra = 0, crb = 0;
    uint8_t icrMask = 0, icrData = 0;
    // Pipelines: bit 0 is "this cycle", higher bits are later cycles.
    uint8_t countA = 0, countB = 0, loadA = 0, loadB = 0, oneShotA = 0, oneShotB = 0;
    uint8_t irqDelay = 0;
    bool irq = false;
    bool pb6 = false, pb7 = false; // timer outputs on PB6/PB7 (toggle state)
    bool pulseA = false, pulseB = false;
    // TOD
    uint8_t tod[4] = {0, 0, 0, 1};    // tenths, seconds, minutes, hours (BCD, bit 7 of hours = PM)
    uint8_t alarm[4] = {0, 0, 0, 0};
    uint8_t latch[4] = {0, 0, 0, 0};
    bool latched = false, todStopped = true;
    uint32_t todCycles = 0; // cycles since the last power-line tick
    uint8_t todTicks = 0;   // power-line ticks since the last tenth
    // Serial
    uint8_t sdr = 0, sdrShift = 0, sdrBits = 0;
    bool sdrPending = false, sdrToggle = false;
};

class Cia6526
{
public:
    enum Flag : uint8_t { TA = 0x01, TB = 0x02, ALRM = 0x04, SP = 0x08, FLG = 0x10 };

    // What the pins read when the CIA does not drive them (DDR bit 0).
    std::function<uint8_t()> readPortA = [] { return uint8_t{0xff}; };
    std::function<uint8_t()> readPortB = [] { return uint8_t{0xff}; };
    // Called whenever the port output changes (for the VIC bank, the serial bus).
    std::function<void()> portsChanged = [] {};

    bool newModel = false;        // 6526A: no IRQ delay
    uint32_t cyclesPerPowerTick = 19705; // clock / 50 Hz (PAL)

    CiaState s;

    void reset();
    void tick();
    uint8_t read(uint8_t reg);
    uint8_t peek(uint8_t reg) const;
    void write(uint8_t reg, uint8_t value);
    // The FLAG pin (falling edge): the serial bus SRQ on CIA1, the user port on CIA2.
    void flag() { setIcr(FLG); }
    bool irq() const { return s.irq; }

    // Port output as seen on the pins (inputs pulled high).
    uint8_t portAOut() const { return static_cast<uint8_t>(s.pra | ~s.ddra); }
    uint8_t portBOut() const;

private:
    void setIcr(uint8_t bits);
    void tickTod();
    void underflowA();
    void underflowB();
    bool todMatchesAlarm() const;
};

} // namespace resident::emu
