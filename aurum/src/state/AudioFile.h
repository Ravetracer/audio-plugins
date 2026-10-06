#pragma once

#include <string>
#include <vector>

namespace aurum {

struct AudioData
{
    double sampleRate = 0.0;
    std::vector<std::vector<float>> channels;
    size_t frames() const { return channels.empty() ? 0 : channels[0].size(); }
};

// Reads WAV (PCM 8/16/24/32, float 32/64, extensible) and AIFF/AIFF-C
// (PCM, 'sowt', 'fl32', 'fl64') files.
bool readAudioFile(const std::string& path, AudioData& out, std::string* error = nullptr);

} // namespace aurum
