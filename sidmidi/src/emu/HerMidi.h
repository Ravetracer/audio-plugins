#pragma once

// HerMIDI: Hermit's MIDI-in interface on the serial bus -- a PIC12F509 with
// its real firmware (third_party/hermidi), wired as the hardware is:
//   GP2 DATA, GP4 ATN, GP5 CLK (open drain on the bus), GP3 the MIDI input
//   (idle high, start bit low, 32 us per bit), GP1 the LED, GP0 pulled up.
// The C64 switches it on by sending its init string over the bus, then reads
// its buffer in the synchronous, asynchronous or direct mode. Bytes from the
// DAW enter the cable at their time and leave it at 31250 baud.

#include <cstdint>
#include <deque>

#include "Pic12F509.h"

namespace sidmidi::emu {

struct HerMidiState
{
    uint64_t frameStart = 0;   // PIC cycle the byte on the cable started
    uint8_t frameByte = 0;
    bool inFrame = false;
};

class HerMidi
{
public:
    HerMidi();
    bool available() const { return pic.hasProgram(); }
    void reset();

    // The bus lines as everybody drives them (true = low), for the PIC's pins.
    bool lineAtn = false, lineClk = false, lineData = false;
    bool pullsClk() const { return pic.drivesLow(5); }
    bool pullsData() const { return pic.drivesLow(2); }
    bool ledOn() const { return pic.drivesLow(1); }

    void runUntil(uint64_t picCycle);
    uint64_t clk() const { return pic.s.clk; }
    // A byte from the DAW, on the cable from PIC cycle `at` (or after the one before).
    void receive(uint8_t byte, uint64_t at);
    void flush() { queue_.clear(); st.inFrame = false; }

    Pic12F509 pic;
    HerMidiState st;
    struct Queued
    {
        uint64_t at;
        uint8_t byte;
    };
    std::deque<Queued>& queue() { return queue_; }
    const std::deque<Queued>& queue() const { return queue_; }

private:
    bool midiLevel(uint64_t t);
    std::deque<Queued> queue_;
};

} // namespace sidmidi::emu
