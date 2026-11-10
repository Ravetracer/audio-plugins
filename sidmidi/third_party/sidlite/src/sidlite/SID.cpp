/*
 * This file is part of libsidplayfp, a SID player engine.
 *
 *  Copyright (C) 2025-2026 Leandro Nini
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

// Based on cRSID lightweight RealSID by Hermit (Mihaly Horvath)

#include "SID.h"

#include "sl_defs.h"
#include "sl_constants.h"
#include "filt_tables.h"
#include "cw_tables.h"
#include "sl_state.h"

#include <algorithm>
#include <iterator>
#include <cstdint>

namespace SIDLite
{

int SID::clock(unsigned int cycles, short* buf)
{
    int i = 0;
    while (cycles > 0)
    {
        short output;
        if (generateSample(cycles, output))
        {
            buf[i] = output;
            i++;
        }
    }
    return i;
}

inline bool SID::generateSample(unsigned int &cycles, short &output)
{
    // Cycle-based part of emulations:

    while (SampleCycleCnt <= s.SampleClockRatio)
    {
        // no more cycles, can't produce output
        if (cycles == 0)
            return false;

        unsigned char InstructionCycles = std::min(7u, cycles);
        SampleCycleCnt += InstructionCycles << CRSID_CLOCK_FRACTIONAL_BITS;
        cycles -= InstructionCycles;

        adsr.clock(InstructionCycles);
    }

    SampleCycleCnt -= s.SampleClockRatio;

    // Samplerate-based part of emulations:

    wg_output_t wg_out = wavgen.clock(&adsr);
    int sample = filter.clock(wg_out.first, wg_out.second);

    // saturation logic on overflow
    if (sample > INT16_MAX)
        sample = INT16_MAX;
    else if (sample < INT16_MIN)
        sample = INT16_MIN;
    output = static_cast<short>(sample);

    nextSampleRatio();
    return true;
}

inline void SID::nextSampleRatio()
{
    RatioRemainder += static_cast<unsigned int>(ExactClockRatio & 0xFFFF);
    s.SampleClockRatio = static_cast<unsigned int>(ExactClockRatio >> 16) + (RatioRemainder >> 16);
    RatioRemainder &= 0xFFFF;
}

void SID::write(int addr, int value)
{
    regs[addr] = value;
}

int SID::read(int addr) const
{
    switch (addr)
    {
        case 0x1B:
            return wavgen.getOsc3();
        case 0x1C:
            return wavgen.getEnv3();
        default:
            return 0;
    }
}

SID::SID() :
    adsr(regs),
    filter(&s, regs),
    wavgen(&s, regs)
{
    setChipModel(model_t::MOS8580);
    reset();
}

void SID::reset()
{
    SampleCycleCnt = 0;
    RatioRemainder = 0;
    s.SampleClockRatio = static_cast<unsigned int>(ExactClockRatio >> 16);

    std::fill(std::begin(regs), std::end(regs), 0);

    adsr.reset();
    filter.reset();
    wavgen.reset();
}

bool SID::setSamplingParameters(double clockFrequency, unsigned int samplingFrequency)
{
    if ((samplingFrequency < 8000) || (samplingFrequency > 48000) || clockFrequency <= 0.0)
        return false;

    filter.rebuildCutoffTables(static_cast<unsigned short>(samplingFrequency));

    // shifting (multiplication) enhances SampleClockRatio precision
    ExactClockRatio = static_cast<unsigned long long>(
        clockFrequency * static_cast<double>(1u << (CRSID_CLOCK_FRACTIONAL_BITS + 16)) / samplingFrequency + 0.5);
    RatioRemainder = 0;
    s.SampleClockRatio = static_cast<unsigned int>(ExactClockRatio >> 16);
    return true;
}

void SID::setChipModel(model_t model)
{
    s.sid8580 = model == model_t::MOS8580;
}

constexpr unsigned char STATE_VERSION = 1;

template<class V> void SID::state(V& v)
{
    v(regs); v(SampleCycleCnt); v(RatioRemainder);
    s.state(v);
    adsr.state(v);
    wavgen.state(v);
    filter.state(v);
}

int SID::stateSize() const
{
    StateSizer sz;
    const_cast<SID*>(this)->state(sz);
    return 1 + sz.size;
}

void SID::saveState(unsigned char* out) const
{
    out[0] = STATE_VERSION;
    StateWriter w{out + 1};
    const_cast<SID*>(this)->state(w);
}

bool SID::restoreState(const unsigned char* in, int size)
{
    if (size != stateSize() || in[0] != STATE_VERSION)
        return false;
    StateReader r{in + 1};
    state(r);
    return true;
}

void SID::setRealSIDmode(bool mode)
{
    s.RealSIDmode = mode;
}

}
