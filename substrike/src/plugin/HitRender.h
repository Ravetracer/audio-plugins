#pragma once

#include <array>
#include <string>
#include <vector>

#include "dsp/Engine.h"

namespace substrike {

// One hit rendered offline: the root note at full velocity, played through a
// private engine with a given set of parameter values and curves. The editor
// draws it and the hit export writes it. Main thread or a worker; it shares
// nothing with the plugin's own engine.
struct HitRender
{
    double sampleRate = 48000.0;
    // The main output, as the host would hear it.
    std::vector<float> left, right;
    // Each lane as it leaves its chain and guard, before the master, whatever
    // its Output routing.
    std::array<std::vector<float>, dsp::kNumLanes> laneLeft, laneRight;
    float peak = 0.0f;
    // The engine went to sleep before the length limit.
    bool complete = false;
};

HitRender renderHit(const std::vector<double>& values, const std::array<dsp::Curve, dsp::kNumCurves>& curves,
                    double sampleRate, double maxSeconds);

// Writes the main output as a 24-bit stereo WAV, optionally peak-normalised
// to -0.3 dBFS. The path is UTF-8.
bool writeHitWav(const std::string& path, const HitRender& hit, bool normalise);

} // namespace substrike
