#include "plugincore/params.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace plugincore {

const ParamDesc *paramByIdIn(const ParamDesc *table, uint32_t count, uint32_t id) {
   if (!table || id >= count)
      return nullptr;
   // The table is laid out in id order; assert that invariant cheaply.
   const ParamDesc *d = &table[id];
   return d->id == id ? d : nullptr;
}

const ParamDesc *paramByKeyIn(const ParamDesc *table, uint32_t count, const char *key) {
   if (!table || !key)
      return nullptr;
   for (uint32_t i = 0; i < count; ++i)
      if (std::strcmp(table[i].key, key) == 0)
         return &table[i];
   return nullptr;
}

double paramToReal(const ParamDesc &desc, double raw) {
   if (raw < desc.min)
      raw = desc.min;
   if (raw > desc.max)
      raw = desc.max;
   switch (desc.kind) {
   case ParamKind::Log:
      return desc.dispMin * std::pow(desc.dispMax / desc.dispMin, raw);
   case ParamKind::Stepped:
   case ParamKind::Enum:
      return std::floor(raw + 0.5);
   case ParamKind::Percent:
   case ParamKind::Linear:
   default:
      return raw;
   }
}

double realToParam(const ParamDesc &desc, double real) {
   double raw = real;
   if (desc.kind == ParamKind::Log) {
      if (real <= 0.0)
         raw = 0.0;
      else
         raw = std::log(real / desc.dispMin) / std::log(desc.dispMax / desc.dispMin);
   }
   if (raw < desc.min)
      raw = desc.min;
   if (raw > desc.max)
      raw = desc.max;
   return raw;
}

namespace {

struct LogDisplay {
   double value;
   int decimals;
   bool scaled; // shown in k, or in seconds for a millisecond parameter
};

// Rounding can push a value across the very boundary that chose its precision:
// 99.96 shows as "100.0" with one decimal, and reading that back shows "100"
// with none, so a host that round-trips the text sees the value drift. Decide
// the format from the number as it will actually be printed, which takes at
// most a couple of passes to settle.
LogDisplay chooseLogDisplay(double real) {
   LogDisplay d{real, 2, false};
   for (int pass = 0; pass < 4; ++pass) {
      d.scaled = real >= 1000.0;
      d.value = d.scaled ? real * 0.001 : real;
      d.decimals = d.scaled ? 2 : (d.value >= 100.0 ? 0 : (d.value >= 10.0 ? 1 : 2));
      const double scale = std::pow(10.0, d.decimals);
      d.value = std::round(d.value * scale) / scale;
      const double shown = d.scaled ? d.value * 1000.0 : d.value;
      if (shown == real)
         break;
      real = shown;
   }
   return d;
}

} // namespace

bool paramValueToText(const ParamDesc &desc, double raw, char *out, uint32_t outSize) {
   if (!out || outSize == 0)
      return false;
   const double real = paramToReal(desc, raw);
   int n = 0;
   switch (desc.kind) {
   case ParamKind::Enum: {
      const uint32_t idx = static_cast<uint32_t>(real < 0 ? 0 : real);
      const char *name = idx < desc.enumCount ? desc.enumNames[idx] : "?";
      n = std::snprintf(out, outSize, "%s", name);
      break;
   }
   case ParamKind::Stepped:
      n = std::snprintf(out, outSize, "%d%s%s", static_cast<int>(real), desc.unit[0] ? " " : "",
                        desc.unit);
      break;
   case ParamKind::Percent:
      n = std::snprintf(out, outSize, "%.1f %%", real * 100.0);
      break;
   case ParamKind::Log: {
      // Milliseconds roll over into seconds; everything else takes a k prefix.
      // "1.20 kms" is not a unit anybody uses.
      const LogDisplay d = chooseLogDisplay(real);
      const char *unit = desc.unit;
      char scaled[16];
      if (d.scaled) {
         if (std::strcmp(desc.unit, "ms") == 0) {
            unit = "s";
         } else {
            std::snprintf(scaled, sizeof(scaled), "k%s", desc.unit);
            unit = scaled;
         }
      }
      n = std::snprintf(out, outSize, "%.*f %s", d.decimals, d.value, unit);
      break;
   }
   case ParamKind::Linear:
   default:
      if (real <= -59.95 && std::strcmp(desc.unit, "dB") == 0)
         n = std::snprintf(out, outSize, "-inf dB");
      else
         n = std::snprintf(out, outSize, "%.2f %s", real, desc.unit);
      break;
   }
   return n > 0 && static_cast<uint32_t>(n) < outSize;
}

bool paramTextToValue(const ParamDesc &desc, const char *text, double *outRaw) {
   if (!text || !outRaw)
      return false;

   if (desc.kind == ParamKind::Enum) {
      for (uint32_t i = 0; i < desc.enumCount; ++i) {
         if (strcasecmp(text, desc.enumNames[i]) == 0) {
            *outRaw = static_cast<double>(i);
            return true;
         }
      }
      // Fall through to numeric parsing so "2" also works.
   }

   char *end = nullptr;
   double v = std::strtod(text, &end);
   if (end == text)
      return false;

   // Accept a k/K multiplier for Hz and ms fields, a bare "s" on a millisecond
   // field (which is how values over a second are displayed) and "ms" on a
   // field that reads in seconds. A kilometre field is already in k, so a "k"
   // there is its own unit. The "ms" test comes first: on a seconds field an
   // "m" is only ever the start of "ms".
   while (*end == ' ')
      ++end;
   const bool kilometres = std::strcmp(desc.unit, "km") == 0;
   const bool seconds = std::strcmp(desc.unit, "s") == 0;
   if ((*end == 'm' || *end == 'M') && seconds && (end[1] == 's' || end[1] == 'S'))
      v *= 0.001; // "500 ms" typed into a field that reads in seconds
   else if ((*end == 'k' || *end == 'K') && !kilometres)
      v *= 1000.0;
   else if ((*end == 'm' || *end == 'M') && kilometres && end[1] != 'i')
      v *= 0.001; // "800 m"
   else if ((*end == 's' || *end == 'S') && std::strcmp(desc.unit, "ms") == 0)
      v *= 1000.0;

   switch (desc.kind) {
   case ParamKind::Percent:
      *outRaw = v * 0.01;
      break;
   case ParamKind::Log:
      *outRaw = realToParam(desc, v);
      break;
   default:
      *outRaw = v;
      break;
   }
   if (*outRaw < desc.min)
      *outRaw = desc.min;
   if (*outRaw > desc.max)
      *outRaw = desc.max;
   return true;
}

} // namespace plugincore
