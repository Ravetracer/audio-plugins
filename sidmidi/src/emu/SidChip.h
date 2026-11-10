#pragma once

// One SID chip behind an engine: reSIDfp (accurate, the reference) or
// SIDLite (light: about 30 times cheaper, envelopes by the cycle, waveforms
// and filter once per output sample). SidBank owns the chips and does the
// clocking, the FIFOs, dormancy and the voice taps the same way for both.

#include <cstdint>
#include <memory>

#include "SidBank.h"

namespace sidmidi::emu {

class SidChip
{
public:
    virtual ~SidChip() = default;
    virtual SidEngine engine() const = 0;

    // Applies a chip's tuning. Only what changed is touched, unless `valid`
    // is false: then everything. An engine ignores what it has no control for.
    virtual void tune(const SidChipConfig& cc, const SidChipConfig& was, bool valid) = 0;
    virtual void setSampling(double clockHz, SidResampling resampling, double sampleRate) = 0;
    virtual void reset() = 0;

    virtual void write(uint8_t reg, uint8_t v) = 0;
    virtual uint8_t read(uint8_t reg) = 0;
    virtual uint8_t peek(uint8_t reg) const = 0;
    // Runs `cycles` CPU cycles; returns the samples written to `buf` at the
    // host rate.
    virtual int clock(unsigned cycles, int16_t* buf) = 0;
    virtual void setPaddle(uint8_t x, uint8_t y) = 0;
    // Bit 0-2: voices 1-3 heard (the voice taps).
    virtual void setVoiceMask(uint8_t mask) = 0;

    // State: this engine and this build only. restoreState returns false and
    // leaves the chip unchanged when `in` is not such a state; `cc` is the
    // chip's configuration, applied before.
    virtual int stateSize() const = 0;
    virtual void saveState(char* out) const = 0;
    virtual bool restoreState(char* in, int size, const SidChipConfig& cc) = 0;
};

std::unique_ptr<SidChip> makeSidChip(SidEngine engine);

} // namespace sidmidi::emu
