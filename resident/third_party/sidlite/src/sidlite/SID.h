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

#ifndef SIDLITE_SID_H
#define SIDLITE_SID_H

#include "ADSR.h"
#include "Filter.h"
#include "WavGen.h"
#include "sl_settings.h"

#include <array>

namespace SIDLite
{

class SID
{
public:
    enum class model_t
    {
        MOS6581,
        MOS8580
    };

public:
    SID();
    void reset();
    void write(int addr, int value);
    int read(int addr) const;
    int clock(unsigned int cycles, short* buf);

    void setChipModel(model_t model);
    void setRealSIDmode(bool mode);
    // Which voices reach the filter and the output (bit 0-2: voice 1-3). A
    // masked voice runs as before (OSC3, ENV3, sync and ring modulation are
    // unchanged) and is heard as a closed envelope.
    void setVoiceMask(unsigned char mask) { s.VoiceMask = mask & 0x07; }
    // clockFrequency in Hz; samplingFrequency 8000..48000 Hz.
    bool setSamplingParameters(double clockFrequency, unsigned int samplingFrequency);

    int getLevel() const { return filter.getLevel(); }

    // State: everything that changes while the chip runs (registers,
    // envelopes, oscillators, filter, the sample clock), not the
    // configuration (model, sampling parameters), which the caller sets
    // first. Only readable by the same build.
    int stateSize() const;
    void saveState(unsigned char* out) const;
    bool restoreState(const unsigned char* in, int size);

private:
    unsigned char regs[0x20] = {0};

    ADSR              adsr;
    Filter            filter;
    WavGen            wavgen;
    settings          s;

    int               SampleCycleCnt;

    // The exact CPU clocks per sample in 1/2^(CRSID_CLOCK_FRACTIONAL_BITS+16)
    // units. Each sample uses the integer part (SampleClockRatio, in
    // 1/2^CRSID_CLOCK_FRACTIONAL_BITS units) plus a carry from the remainder,
    // so the average ratio is exact: no drift of the sample count against
    // the clock, and no pitch error.
    unsigned long long ExactClockRatio = 0;
    unsigned int      RatioRemainder = 0;

private:
    inline bool generateSample(unsigned int &cycles, short &output);
    inline void nextSampleRatio();
    template<class V> void state(V& v);
};

}

#endif // SIDLITE_SID_H
