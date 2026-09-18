#pragma once

// Shorthand for building a ParamDesc table. Included by a plugin's params.cpp
// and #undef'd again straight after the table, so these never leak further.
//
// clang-format off
#define LIN(id, key, name, mod, lo, hi, def, unit, tip)                                            \
   { id, key, name, mod, lo, hi, def, ParamKind::Linear, 0, 0, unit, nullptr, 0, tip }
#define PCT(id, key, name, mod, def, tip)                                                          \
   { id, key, name, mod, 0.0, 1.0, def, ParamKind::Percent, 0, 0, "%", nullptr, 0, tip }
#define BIPCT(id, key, name, mod, def, tip)                                                        \
   { id, key, name, mod, -1.0, 1.0, def, ParamKind::Percent, 0, 0, "%", nullptr, 0, tip }
#define LOG(id, key, name, mod, def, dlo, dhi, unit, tip)                                          \
   { id, key, name, mod, 0.0, 1.0, def, ParamKind::Log, dlo, dhi, unit, nullptr, 0, tip }
#define STEP(id, key, name, mod, lo, hi, def, unit, tip)                                           \
   { id, key, name, mod, lo, hi, def, ParamKind::Stepped, 0, 0, unit, nullptr, 0, tip }
#define ENUM(id, key, name, mod, def, names, tip)                                                  \
   {                                                                                               \
      id, key, name, mod, 0.0, static_cast<double>(sizeof(names) / sizeof(names[0]) - 1), def,     \
         ParamKind::Enum, 0, 0, "", names, sizeof(names) / sizeof(names[0]), tip                   \
   }

// The full control surface. Ranges chosen so that a plain linear fader in the
// host's generic UI lands somewhere useful across its whole travel. Log
// defaults are the raw 0..1 position: log(def / lo) / log(hi / lo).
// clang-format on
