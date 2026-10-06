#pragma once

#include <map>
#include <string>
#include <vector>

namespace substrike {

// Plain-text plugin state and preset format:
//
//   [Substrike]
//   format=1
//   name=...                  (optional metadata)
//   [Parameters]
//   l1.body.pitch_start=350   (plain units: Hz, ms, dB, %, degrees)
//   root_note=C1              (enumerations by label)
//
// Values are written in their own units rather than as knob positions, so a
// preset stays correct when a range is widened later and can be written by
// hand. Unknown keys are ignored and missing parameters fall back to their
// defaults.
struct StateDocument
{
    std::map<std::string, std::string> meta;
    std::vector<double> values; // stored values, indexed like ParamTable
};

std::string serializeState(const StateDocument& doc);
// Returns false if the text is not a Substrike document.
bool parseState(const std::string& text, StateDocument& doc);

std::vector<double> defaultValues();

} // namespace substrike
