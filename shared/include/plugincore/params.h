#pragma once

#include <cstdint>

// The parameter model shared by every plugin built on it.
//
// A plugin owns its own ParamId enum and its own table of ParamDesc; everything
// that operates on a descriptor -- range mapping, display formatting, text
// parsing -- is the same everywhere and lives here.

namespace plugincore {

enum class ParamKind {
   Linear,  // host value is the real value
   Percent, // host value 0..1, displayed as 0..100 %
   Log,     // host value 0..1, mapped exponentially onto [dispMin, dispMax]
   Stepped, // integer host value
   Enum     // integer host value with names
};

enum FilterKind { kFilterLowpass = 0, kFilterBandpass, kFilterHighpass, kFilterNotch };

struct ParamDesc {
   uint32_t id;
   const char *key;    // stable machine key used in preset files
   const char *name;   // human readable name shown by the host
   const char *module; // host-side grouping, e.g. "Rain", "Strike"
   double min;
   double max;
   double def;
   ParamKind kind;
   double dispMin; // only meaningful for Log
   double dispMax;
   const char *unit;
   const char *const *enumNames;
   uint32_t enumCount;
   const char *tip; // one-line explanation, shown in the plugin's help line
};

// Table lookups. A plugin's own paramById()/paramByKey() are thin wrappers
// that pass their table and its size.
const ParamDesc *paramByIdIn(const ParamDesc *table, uint32_t count, uint32_t id);
const ParamDesc *paramByKeyIn(const ParamDesc *table, uint32_t count, const char *key);

// Convert a raw host-facing parameter value into the real-world quantity used
// by the DSP (Hz, ms, dB, km, ratio, ...).
double paramToReal(const ParamDesc &desc, double raw);

// Inverse of paramToReal, used when parsing preset files written in
// real-world units.
double realToParam(const ParamDesc &desc, double real);

// Formats `raw` for display. Returns false if the buffer was too small.
bool paramValueToText(const ParamDesc &desc, double raw, char *out, uint32_t outSize);
bool paramTextToValue(const ParamDesc &desc, const char *text, double *outRaw);

} // namespace plugincore
