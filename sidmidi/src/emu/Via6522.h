#pragma once

// MOS 6522 VIA, as the 1541 uses two of them: the ports with their direction
// registers, timer 1 (one-shot and free-running), timer 2 (one-shot), CA1/CB1
// edge interrupts, CA2/CB2 as outputs, port A latching on CA1, and the
// interrupt flag and enable registers. The shift register is stored but does
// not shift (nothing in a 1541 uses it).

#include <cstdint>
#include <functional>

namespace sidmidi::emu {

struct ViaState
{
    uint8_t ora = 0, orb = 0, ddra = 0, ddrb = 0;
    uint16_t t1 = 0xffff, t1Latch = 0xffff, t2 = 0xffff;
    uint8_t t2LatchLo = 0xff;
    bool t1Armed = false, t2Armed = false, t1Reload = false;
    uint8_t sr = 0, acr = 0, pcr = 0, ifr = 0, ier = 0;
    bool ca1 = false, cb1 = false;
    uint8_t paLatch = 0xff;
};

class Via6522
{
public:
    enum : uint8_t { CA2 = 0x01, CA1 = 0x02, SR = 0x04, CB2 = 0x08, CB1 = 0x10, T2 = 0x20, T1 = 0x40 };

    // The pins as the outside world drives them (inputs pulled high).
    std::function<uint8_t()> readPortA = [] { return uint8_t{0xff}; };
    std::function<uint8_t()> readPortB = [] { return uint8_t{0xff}; };
    // Called after a write that may have changed an output.
    std::function<void()> outputsChanged = [] {};

    ViaState s;

    void reset() { s = ViaState{}; }
    void tick()
    {
        if (s.t1Reload) {
            s.t1 = s.t1Latch;
            s.t1Reload = false;
        } else if (s.t1 == 0) {
            if (s.t1Armed) {
                s.ifr |= T1;
                if (!(s.acr & 0x40))
                    s.t1Armed = false;
            }
            if (s.acr & 0x40)
                s.t1Reload = true;
            else
                s.t1 = 0xffff;
        } else {
            s.t1--;
        }
        if (!(s.acr & 0x20)) { // timed mode; pulse counting on PB6 is unused
            if (s.t2 == 0 && s.t2Armed) {
                s.ifr |= T2;
                s.t2Armed = false;
            }
            s.t2--;
        }
    }
    uint8_t read(uint8_t reg);
    uint8_t peek(uint8_t reg) const;
    void write(uint8_t reg, uint8_t v);
    bool irq() const { return (s.ifr & s.ier & 0x7f) != 0; }

    // Input edges.
    void setCA1(bool level);
    void setCB1(bool level);
    // A pulse on CA1 in its active direction (the 1541's BYTE READY).
    void pulseCA1();

    // Port outputs as driven (bits with DDR 0 read as 1).
    uint8_t portAOut() const { return static_cast<uint8_t>(s.ora | ~s.ddra); }
    uint8_t portBOut() const { return static_cast<uint8_t>(s.orb | ~s.ddrb); }
    // CA2/CB2 in their manual output modes.
    bool ca2High() const { return (s.pcr & 0x0e) != 0x0c; }
    bool cb2High() const { return (s.pcr & 0xe0) != 0xc0; }
};

} // namespace sidmidi::emu
