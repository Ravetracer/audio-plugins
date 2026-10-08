#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include "dsp/Engine.h"

namespace substrike {

// Plain-text plugin state and preset format:
//
//   [Substrike]
//   format=1
//   name=...                  (optional metadata)
//   [Parameters]
//   l1.body.pitch_start=350   (plain units: Hz, ms, dB, %, degrees)
//   root_note=C1              (enumerations by label)
//   [Curves]
//   l1.pitch=0,1,0;0.4,0.2,0.5;1,0,0   (x,y,curvature per point)
//
// Values are written in their own units rather than as knob positions, so a
// preset stays correct when a range is widened later and can be written by
// hand. Unknown keys are ignored and missing parameters fall back to their
// defaults. The breakpoint curves are state rather than parameters (their
// macros are the parameters); a missing curve is the default single segment.
struct StateDocument
{
    std::map<std::string, std::string> meta;
    std::vector<double> values; // stored values, indexed like ParamTable
    std::array<dsp::Curve, dsp::kNumCurves> curves{};
};

// A curve as the state writes it, and back. Parsing repairs what it can
// (see dsp::Curve::set) and fails only on text that is not a point list.
std::string curveToText(const dsp::Curve& c);
bool curveFromText(const std::string& text, dsp::Curve& c);
// The state key of a curve: "l1.pitch", "l1.amp", ...
std::string curveKey(int index);

std::string serializeState(const StateDocument& doc);
// Returns false if the text is not a Substrike document.
bool parseState(const std::string& text, StateDocument& doc);

std::vector<double> defaultValues();

} // namespace substrike
