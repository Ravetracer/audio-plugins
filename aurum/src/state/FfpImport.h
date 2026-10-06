#pragma once

#include <string>

#include "StateIO.h"

namespace aurum {

// Converts an external text preset (.ffp, "Signature=FR2p") into an Aurum
// state document. Only the user-visible settings are mapped; the result is a
// starting point, not an exact match, because the reverb algorithms differ.
// Surround-only keys are ignored.
bool importFfpPreset(const std::string& text, StateDocument& out, std::string* error = nullptr);

} // namespace aurum
