#pragma once

#include <map>
#include <string>
#include <vector>

namespace aurum {

// Plain-text plugin state / preset format:
//
//   [Aurum]
//   format=1
//   name=...            (optional metadata)
//   [Parameters]
//   space=0.6
//   ...
//
// Unknown keys are ignored and missing parameters fall back to defaults, so
// the format stays compatible when parameters are added.
struct StateDocument
{
    std::map<std::string, std::string> meta;
    std::vector<double> values; // indexed like ParamTable
};

std::string serializeState(const StateDocument& doc);
// Returns false if the text is not an Aurum document.
bool parseState(const std::string& text, StateDocument& doc);

// Fill a value vector with parameter defaults.
std::vector<double> defaultValues();

} // namespace aurum
