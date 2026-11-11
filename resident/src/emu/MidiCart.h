#pragma once

// The C64 MIDI cartridges: a Motorola 6850 ACIA in the I/O pages, wired to
// IRQ or NMI, at the addresses each maker chose.
//
// The receive line carries one 10-bit frame per byte at 31250 baud, so bytes
// arrive no faster than a real cable delivers them and a program that is too
// slow to read them gets an overrun, as on the hardware. What the program
// transmits leaves after its frame time and is handed to the plugin with the
// cycle it finished on.
//
// After a master reset (control = %xxxxxx11) of an ACIA held in reset since
// power-on the status register reads 0, and TDRE comes up once a non-reset
// control word is written: Cynthcart's autodetection depends on exactly that.
// An ACIA that was running and is reset again keeps TDRE readable, as in
// VICE: Station64's Sequential/Namesoft test waits for it (MidiCart::write).

#include <cstdint>
#include <vector>

#include "FixedQueue.h"

namespace resident::emu {

enum class MidiInterface : uint8_t
{
    None = 0,
    Datel,      // Datel / Siel / JMS / C-Lab / Kerberos / TeensyROM
    Sequential,
    Passport,   // Passport / Syntech
    Namesoft,
    Maplin,
    MoogSP,     // Moog Song Producer
    HerMIDI,    // on the serial bus (HerMidi.h), not a cartridge
    Count
};

const char* midiInterfaceName(MidiInterface m);

struct MidiByte
{
    uint64_t clk;
    uint8_t byte;
};

struct MidiCartState
{
    uint8_t cr = 0x03, sr = 0x00, rdr = 0, tdr = 0;
    bool inReset = true;
    bool txFull = false;      // TDR holds a byte not yet in the shifter
    bool txShifting = false;
    uint8_t txShift = 0;
    uint64_t txDoneClk = 0;   // when the byte in the shifter has left
    uint64_t rxLineFreeClk = 0; // when the receive line can complete its next frame
};

class MidiCart
{
public:
    MidiInterface type = MidiInterface::Datel;
    uint32_t cyclesPerFrame = 315; // clock / 3125 (10 bits at 31250 baud)

    MidiCartState s;

    void reset();
    // True when the address is one of this cartridge's registers.
    bool decodes(uint16_t addr) const;
    uint8_t read(uint16_t addr, uint64_t clk);
    uint8_t peek(uint16_t addr) const;
    void write(uint16_t addr, uint8_t v, uint64_t clk);
    void tick(uint64_t clk)
    {
        if ((!rx_.empty() && clk >= rx_.front().clk) || (s.txShifting && clk >= s.txDoneClk) )
            service(clk);
    }

    bool irqLine() const { return irqAsserted() && !isNmi(); }
    bool nmiLine() const { return irqAsserted() && isNmi(); }
    bool isNmi() const { return type == MidiInterface::Namesoft; }

    // A byte from the DAW, arriving on the cable at `clk`. Its frame completes
    // at the earliest one frame time later, and after the frame before it.
    void receive(uint8_t byte, uint64_t clk);
    // Bytes the program transmitted since the last call, oldest first. Appends
    // only up to out.capacity(), so a reserved vector never reallocates; what
    // does not fit is dropped.
    void takeTransmitted(std::vector<MidiByte>& out);
    size_t pendingReceive() const { return rx_.size(); }
    // Drops everything in flight (a reset of the machine).
    void flush();

    // About 1.3 s of cable at 31250 baud; a byte that arrives with the
    // queue full is dropped.
    using RxQueue = FixedQueue<MidiByte, 4096>;
    using TxQueue = FixedQueue<MidiByte, 1024>;

    // For the snapshot.
    const RxQueue& rxQueue() const { return rx_; }
    RxQueue& rxQueue() { return rx_; }

private:
    void service(uint64_t clk);
    bool irqAsserted() const;
    uint16_t base() const;
    bool fourRegisters() const;

    RxQueue rx_;
    TxQueue tx_;
};

} // namespace resident::emu
