#pragma once

#include <array>
#include <cmath>

#include "Math.h"

namespace aurum::dsp {

// Parameters describing one point on the Space control. The Space knob moves
// continuously between the anchors below; all values are interpolated.
struct Room
{
    const char* name = "";
    double t60;        // nominal reverberation time (s)
    double size;       // mean late line length (ms)
    double spread;     // ratio between longest and shortest line
    double erLength;   // span of the early reflection pattern (ms)
    double erStart;    // first reflection delay at distance 0 (ms)
    double erLevel;    // early reflection level (dB)
    double lfMult;     // T60 multiplier at low frequencies
    double hfMult;     // T60 multiplier at high frequencies
    double hfFreq;     // corner of the high frequency decay (Hz)
    double diffusion;  // input diffusion amount (0..1)
};

inline constexpr int kNumRooms = 11;

// Anchors sit at Space = 0.0, 0.1, ... 1.0; t60 is interpolated linearly
// between them. This scale also makes imported .ffp Space values map 1:1.
inline const std::array<Room, kNumRooms>& roomAnchors()
{
    //                         name           t60   size spread erLen erStart erLvl  lf    hf    hfF    diff
    static const std::array<Room, kNumRooms> rooms = {{
        {"Ambience",            0.20,  5.0, 2.4,  9.0, 0.8, -1.0, 1.00, 0.85, 6000.0, 0.55},
        {"Small Room",          0.40,  8.0, 2.6, 16.0, 1.2, -1.5, 1.05, 0.80, 5500.0, 0.60},
        {"Medium Room",         0.75, 13.0, 2.9, 26.0, 1.8, -2.2, 1.08, 0.72, 4700.0, 0.64},
        {"Large Room",          1.25, 20.0, 3.0, 40.0, 2.6, -3.0, 1.15, 0.65, 4200.0, 0.68},
        {"Chamber",             1.85, 26.0, 3.2, 48.0, 3.0, -3.0, 1.20, 0.60, 4000.0, 0.70},
        {"Small Hall",          2.50, 33.0, 3.2, 58.0, 3.6, -3.5, 1.25, 0.58, 3800.0, 0.72},
        {"Concert Hall",        3.20, 42.0, 3.3, 70.0, 4.4, -4.0, 1.30, 0.55, 3600.0, 0.74},
        {"Large Hall",          4.00, 52.0, 3.4, 84.0, 5.2, -4.5, 1.35, 0.50, 3400.0, 0.75},
        {"Arena",               5.20, 64.0, 3.4, 100.0, 6.5, -5.0, 1.35, 0.45, 3200.0, 0.76},
        {"Church",              7.00, 76.0, 3.5, 115.0, 7.5, -5.0, 1.45, 0.45, 3000.0, 0.78},
        {"Cathedral",          10.00, 90.0, 3.6, 135.0, 9.0, -5.5, 1.50, 0.42, 2800.0, 0.80},
    }};
    return rooms;
}

// Space value of the given anchor (anchors are evenly spaced).
inline double roomAnchorPosition(int index) { return static_cast<double>(index) / (kNumRooms - 1); }

inline Room roomAt(double space)
{
    const auto& rooms = roomAnchors();
    const double pos = clamp(space, 0.0, 1.0) * (kNumRooms - 1);
    const int i0 = std::min(static_cast<int>(pos), kNumRooms - 2);
    const double t = pos - i0;
    const Room& a = rooms[static_cast<size_t>(i0)];
    const Room& b = rooms[static_cast<size_t>(i0 + 1)];
    Room r;
    r.name = t < 0.5 ? a.name : b.name;
    r.t60 = lerp(a.t60, b.t60, t);
    r.size = gerp(a.size, b.size, t);
    r.spread = lerp(a.spread, b.spread, t);
    r.erLength = gerp(a.erLength, b.erLength, t);
    r.erStart = gerp(a.erStart, b.erStart, t);
    r.erLevel = lerp(a.erLevel, b.erLevel, t);
    r.lfMult = gerp(a.lfMult, b.lfMult, t);
    r.hfMult = gerp(a.hfMult, b.hfMult, t);
    r.hfFreq = gerp(a.hfFreq, b.hfFreq, t);
    r.diffusion = lerp(a.diffusion, b.diffusion, t);
    return r;
}

// Inverse of roomAt(space).t60 (monotonic), by bisection.
inline double spaceForT60(double seconds)
{
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 40; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        if (roomAt(mid).t60 < seconds)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5 * (lo + hi);
}

} // namespace aurum::dsp
