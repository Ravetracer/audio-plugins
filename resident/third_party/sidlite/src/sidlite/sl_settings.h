/*
 * This file is part of libsidplayfp, a SID player engine.
 *
 *  Copyright (C) 2026 Leandro Nini
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

#ifndef SIDLITE_SETTINGS_H
#define SIDLITE_SETTINGS_H

namespace SIDLite
{

class SID;

class settings
{
    friend class SID;

public:
    inline bool is8580() const { return sid8580; }
    inline unsigned int getSampleClockRatio() const { return SampleClockRatio; }
    inline bool getRealSIDmode() const { return RealSIDmode; }
    inline unsigned char getVoiceMask() const { return VoiceMask; }

    template<class V> void state(V& v) { v(SampleClockRatio); }

private:
    unsigned int   SampleClockRatio = 0; // ratio of CPU-clock and samplerate, this sample's
    bool           sid8580     = true;  // SID model 8580 or 6581
    bool           RealSIDmode = true;
    unsigned char  VoiceMask   = 0x07;  // bit 0-2: voice 1-3 heard
};

}

#endif
