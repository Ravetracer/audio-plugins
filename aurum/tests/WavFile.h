#pragma once

#include <string>
#include <vector>

// Minimal WAV writer used by the test tools (32-bit float, interleaved).
bool writeWav(const std::string& path, const std::vector<float>& left, const std::vector<float>& right, int rate);
