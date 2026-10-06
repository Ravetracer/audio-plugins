#pragma once

#include <string>
#include <vector>

#include "AudioFile.h"
#include "StateIO.h"

namespace aurum {

struct IrReport
{
    std::vector<double> bandFreq;
    std::vector<double> bandT60;  // measured, seconds (0 = not measurable)
    std::vector<double> bandLevel; // initial spectrum, dB re mid bands
    double t60Mid = 0.0;
    double predelayMs = 0.0;
    double c50 = 0.0;
    double correlation = 1.0;
};

// Analyses a transient impulse response and derives settings that match its
// decay (Space, Decay Rate EQ), spectrum (Post EQ), width, distance and
// predelay. `current` supplies values that are kept (e.g. Mix).
bool importImpulseResponse(const AudioData& ir, const std::vector<double>& current, StateDocument& out,
                           IrReport* report = nullptr, std::string* error = nullptr);

} // namespace aurum
