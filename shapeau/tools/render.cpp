// Shapeau's offline renderer and self-test.
//
//   shapeau-render --selftest [--plugin build/Shapeau.clap]
//   shapeau-render --list
//   shapeau-render --plugin build/Shapeau.clap --preset p.shapeau --out out.wav
//                    [--seconds 4] [--tail 1] [--rate 48000]
//   shapeau-render --plugin build/Shapeau.clap --all --outdir dir [--seconds 4]
//
// The self-test has two halves. The first runs the engine directly, because
// what it measures -- the flat sum of the band split, the gain a curve really
// produces, a filter's attenuation, a shaper's distortion, a read head's
// position -- is not visible from outside the plugin without an analysis
// nobody would trust more than the engine itself. The second loads the built
// .clap through the CLAP API, so the glue is exercised as a host sees it.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <clap/clap.h>

#if defined(_WIN32)
#   include <windows.h>
#   define RD_DLOPEN(p) (void *)LoadLibraryA(p)
#   define RD_DLSYM(h, s) (void *)GetProcAddress((HMODULE)(h), s)
#   define RD_DLCLOSE(h) FreeLibrary((HMODULE)(h))
#else
#   include <dlfcn.h>
#   define RD_DLOPEN(p) dlopen(p, RTLD_NOW | RTLD_LOCAL)
#   define RD_DLSYM(h, s) dlsym(h, s)
#   define RD_DLCLOSE(h) dlclose(h)
#endif

#include <filesystem>
#include <fstream>
#include <sstream>

#include "dsp/curve.h"
#include "dsp/engine.h"
#include "params.h"
#include "shapeau.h"

#ifndef SHAPEAU_PRESET_DIR
#   define SHAPEAU_PRESET_DIR "presets"
#endif

namespace {

int gPass = 0;
int gFail = 0;

void check(bool ok, const char *what) {
   std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
   if (ok)
      ++gPass;
   else
      ++gFail;
}

// ---------------------------------------------------------------- signal math

constexpr double kPi = 3.14159265358979323846;

double rms(const std::vector<float> &x) {
   double sum = 0.0;
   for (float v : x)
      sum += static_cast<double>(v) * v;
   return x.empty() ? 0.0 : std::sqrt(sum / x.size());
}

// The same, with the first tenth left out: a module's smoothing starts from
// its previous value and takes a few milliseconds to arrive, and measuring the
// attack as if it were the steady state would fail every check about a shape.
double rmsTail(const std::vector<float> &x) {
   const size_t from = x.size() / 10;
   if (from >= x.size())
      return 0.0;
   double sum = 0.0;
   for (size_t i = from; i < x.size(); ++i)
      sum += static_cast<double>(x[i]) * x[i];
   return std::sqrt(sum / (x.size() - from));
}

double differenceDb(const std::vector<float> &a, const std::vector<float> &b) {
   double sum = 0.0;
   double ref = 0.0;
   const size_t n = std::min(a.size(), b.size());
   for (size_t i = 0; i < n; ++i) {
      const double d = static_cast<double>(a[i]) - b[i];
      sum += d * d;
      ref += static_cast<double>(a[i]) * a[i];
   }
   if (ref <= 0.0)
      return -300.0;
   return 10.0 * std::log10(std::max(1.0e-30, sum / ref));
}

// Energy at the fundamental against everything else: a plain total-harmonic-
// distortion reading, done with a naive DFT at the first few harmonics, which
// is enough to tell a shaper from a straight wire.
double thdAt(const std::vector<float> &x, double sr, double f0) {
   auto power = [&](double f) {
      double re = 0.0, im = 0.0;
      const double w = 2.0 * kPi * f / sr;
      for (size_t i = 0; i < x.size(); ++i) {
         re += x[i] * std::cos(w * i);
         im += x[i] * std::sin(w * i);
      }
      return re * re + im * im;
   };
   const double fundamental = power(f0);
   double harmonics = 0.0;
   for (int h = 2; h <= 7; ++h)
      harmonics += power(f0 * h);
   if (fundamental <= 0.0)
      return 0.0;
   return std::sqrt(harmonics / fundamental);
}

struct Run {
   std::vector<float> l, r;
};

Run runEngine(const shapeau::EngineParams &params, const shapeau::CurveSet &curves,
              const std::vector<float> &inL, const std::vector<float> &inR, double sr,
              double bpm = 130.0) {
   shapeau::Engine engine;
   engine.prepare(sr, 512);
   engine.setParams(params);
   engine.publishCurves(curves);
   Run out;
   out.l = inL;
   out.r = inR;
   constexpr uint32_t kBlock = 256;
   double beats = 0.0;
   for (size_t pos = 0; pos < out.l.size(); pos += kBlock) {
      const uint32_t n = static_cast<uint32_t>(std::min<size_t>(kBlock, out.l.size() - pos));
      engine.process(out.l.data() + pos, out.r.data() + pos, n, beats, bpm);
      beats += static_cast<double>(n) * bpm / 60.0 / sr;
   }
   return out;
}

shapeau::EngineParams neutralParams() {
   shapeau::EngineParams p;
   p.inputGain = 1.0f;
   p.outputGain = 1.0f;
   p.mix = 1.0f;
   p.splitLowHz = 200.0f;
   p.splitHighHz = 2000.0f;
   p.volOn = true;
   p.volRate = shapeau::kRate4;
   p.volSmooth = 0.0f;
   return p;
}

// ------------------------------------------------------------- WAV writing

bool writeWav(const std::string &path, const std::vector<float> &l, const std::vector<float> &r,
              double sr) {
   std::ofstream f(path, std::ios::binary);
   if (!f)
      return false;
   const uint32_t frames = static_cast<uint32_t>(std::min(l.size(), r.size()));
   const uint32_t dataBytes = frames * 4;
   const uint32_t rate = static_cast<uint32_t>(sr);
   auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); };
   auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char *>(&v), 2); };
   f.write("RIFF", 4);
   u32(36 + dataBytes);
   f.write("WAVEfmt ", 8);
   u32(16);
   u16(1);
   u16(2);
   u32(rate);
   u32(rate * 4);
   u16(4);
   u16(16);
   f.write("data", 4);
   u32(dataBytes);
   for (uint32_t i = 0; i < frames; ++i) {
      for (int c = 0; c < 2; ++c) {
         const float v = c == 0 ? l[i] : r[i];
         const float clamped = std::min(1.0f, std::max(-1.0f, v));
         const int16_t s = static_cast<int16_t>(std::lround(clamped * 32767.0f));
         f.write(reinterpret_cast<const char *>(&s), 2);
      }
   }
   return static_cast<bool>(f);
}

// ------------------------------------------------------------------ the tests

void selftestEngine() {
   const double sr = 48000.0;
   const size_t n = static_cast<size_t>(sr * 0.5);
   std::vector<float> silence(n, 0.0f), sine(n), noise(n);
   for (size_t i = 0; i < n; ++i) {
      sine[i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * 220.0 * i / sr));
      const uint32_t s = static_cast<uint32_t>(i * 2654435761u);
      noise[i] = (static_cast<float>(s >> 8) / 8388608.0f) - 1.0f;
      noise[i] *= 0.2f;
   }

   // 1. A shape survives being written and read back.
   {
      shapeau::CurveSet set;
      shapeau::resetCurves(set);
      shapeau::Curve &c = set.curve[shapeau::kModuleVolume][shapeau::kBandMid];
      c.setFlat(1.0f);
      c.addPoint(0.25f, 0.0f, 2);
      c.addPoint(0.5f, 0.5f, 1);
      c.addPoint(0.75f, 1.0f, 0);
      const std::string text = c.format();
      shapeau::Curve back;
      const bool parsed = back.parse(text);
      double worst = 0.0;
      for (int i = 0; i < 128; ++i) {
         const float phase = static_cast<float>(i) / 128.0f;
         const float delta = c.valueAt(phase) - back.valueAt(phase);
         worst = std::max(worst, static_cast<double>(std::fabs(delta)));
      }
      check(parsed && back.count() == c.count() && worst < 1.0e-3,
            "a shape written as text reads back with the same points and values");
   }

   // 2. A hard segment is a straight line; a soft one passes through the middle.
   {
      shapeau::Curve hard;
      hard.setFlat(0.0f);
      hard.addPoint(1.0f, 1.0f, 0);
      shapeau::Curve soft;
      soft.setFlat(0.0f);
      soft.addPoint(0.0f, 0.0f, 2);
      soft.addPoint(1.0f, 1.0f, 2);
      check(std::fabs(hard.valueAt(0.5f) - 0.5f) < 1.0e-4 &&
               std::fabs(soft.valueAt(0.25f) - 0.156f) < 0.02f,
            "the interpolation follows the point weights");
   }

   // 2b. A bent segment keeps its endpoints and leaves the straight line.
   {
      shapeau::Curve straight;
      straight.setFlat(0.0f);
      straight.addPoint(1.0f, 1.0f, 0);
      shapeau::Curve bent = straight;
      bent.setBend(0, 0.8f);
      check(std::fabs(bent.valueAt(0.0f)) < 1.0e-5f &&
               std::fabs(bent.valueAt(1.0f) - 1.0f) < 1.0e-5f &&
               bent.valueAt(0.25f) > straight.valueAt(0.25f) + 0.1f &&
               bent.valueAt(0.75f) > straight.valueAt(0.75f) + 0.1f,
            "a segment bend bows the curve without moving its ends");

      // The bend is part of the text form, and a three-number point -- the
      // format before bend existed -- still loads on a straight segment.
      const std::string text = bent.format();
      shapeau::Curve back;
      const bool parsed = back.parse(text);
      shapeau::Curve oldFormat;
      const bool oldParsed = oldFormat.parse("0.0000 0.0000 0, 1.0000 1.0000 0");
      check(parsed && std::fabs(back.point(0).bend - 0.8f) < 1.0e-3f &&
               oldParsed && std::fabs(oldFormat.point(0).bend) < 1.0e-6f,
            "a segment bend survives the text form, and old points load straight");
   }

   // 2c. A step shape is flat inside a cell and jumps at its boundary.
   {
      shapeau::Curve steps;
      steps.setFlat(0.0f);
      steps.addPoint(1.0f, 1.0f, 0);
      steps.fillSteps(16);
      const bool flatCell =
         std::fabs(steps.valueAt(0.03f) - steps.valueAt(0.055f)) < 1.0e-4f &&
         std::fabs(steps.valueAt(0.20f) - steps.valueAt(0.22f)) < 1.0e-4f;
      const bool movesOn = std::fabs(steps.valueAt(0.03f) - steps.valueAt(0.09f)) > 0.04f;
      const std::string text = steps.format();
      shapeau::Curve back;
      const bool parsed = back.parse(text);
      check(flatCell && movesOn && parsed && back.count() == steps.count(),
            "a step shape is flat within a cell and jumps at the cell edge");
      check(steps.count() <= shapeau::kMaxCurvePoints,
            "sixteen steps fits inside the curve's point budget");
   }

   // 3. The three bands sum flat. A Linkwitz-Riley fourth-order split sums to
   // an allpass, not to the input sample for sample -- the phase turns, the
   // magnitude does not -- so this measures levels, which is what "transparent"
   // means to an ear.
   {
      bool flat = true;
      double worst = 0.0;
      for (double hz : {80.0, 400.0, 1200.0, 5000.0}) {
         std::vector<float> x(n);
         for (size_t i = 0; i < n; ++i)
            x[i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * hz * i / sr));
         shapeau::CurveSet curves;
         shapeau::resetCurves(curves);
         const auto out = runEngine(neutralParams(), curves, x, x, sr);
         const double db = 20.0 * std::log10(std::max(1.0e-9, rmsTail(out.l) / rmsTail(x)));
         worst = std::max(worst, std::fabs(db));
         if (std::fabs(db) > 0.5)
            flat = false;
      }
      check(flat, "a flat unity shape is level-transparent through the three-band split");
      std::printf("      worst band-sum error %.2f dB\n", worst);
   }

   // 4. The volume shape's value is a gain, and its middle is about -12 dB.
   {
      shapeau::CurveSet curves;
      shapeau::resetCurves(curves);
      for (int b = 0; b < shapeau::kNumBands; ++b)
         curves.curve[shapeau::kModuleVolume][b].setFlat(0.5f);
      const auto out = runEngine(neutralParams(), curves, sine, sine, sr);
      const double ratio = rms(out.l) / rms(sine);
      check(std::fabs(ratio - 0.25) < 0.01, "a shape at 50 % is a quarter of the level (-12 dB)");
   }

   // 5. A shape at zero is silence.
   {
      shapeau::CurveSet curves;
      shapeau::resetCurves(curves);
      for (int b = 0; b < shapeau::kNumBands; ++b)
         curves.curve[shapeau::kModuleVolume][b].setFlat(0.0f);
      const auto out = runEngine(neutralParams(), curves, sine, sine, sr);
      check(rmsTail(out.l) < 1.0e-5, "a shape at zero is silence");
   }

   // 6. The filter opens and closes with the cutoff the knob sets.
   {
      auto low = [&](double hz) {
         std::vector<float> x(n);
         for (size_t i = 0; i < n; ++i)
            x[i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * hz * i / sr));
         return x;
      };
      shapeau::CurveSet curves;
      shapeau::resetCurves(curves);
      shapeau::EngineParams p;
      p.fltOn = true;
      p.fltCutHz = 500.0f;
      p.fltDepthOct = 5;
      p.fltRes = 0.2f;
      p.bandOn[0] = p.bandOn[1] = p.bandOn[2] = true;
      const auto passLow = runEngine(p, curves, low(200.0), low(200.0), sr);
      const auto stopHigh = runEngine(p, curves, low(8000.0), low(8000.0), sr);
      const double lowDb = 20.0 * std::log10(rms(passLow.l) / rms(low(200.0)));
      const double highDb = 20.0 * std::log10(rms(stopHigh.l) / rms(low(8000.0)));
      check(std::fabs(lowDb) < 1.0 && highDb < -20.0,
            "a lowpass at 500 Hz passes 200 Hz and stops 8 kHz");
   }

   // 7. The drive shapes the signal, and at zero drive it does not. Measured
   // at 700 Hz, which sits inside one band: a tone on a crossover is split in
   // two before it reaches the shaper and each half is quieter than the tone.
   std::vector<float> tone700(n);
   for (size_t i = 0; i < n; ++i)
      tone700[i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * 700.0 * i / sr));
   {
      shapeau::CurveSet curves;
      shapeau::resetCurves(curves);
      shapeau::EngineParams p;
      p.drvOn = true;
      p.drvDrive = 0.0f;
      const auto clean = runEngine(p, curves, tone700, tone700, sr);
      p.drvDrive = 0.9f;
      const auto driven = runEngine(p, curves, tone700, tone700, sr);
      const double cleanThd = thdAt(clean.l, sr, 700.0);
      const double drivenThd = thdAt(driven.l, sr, 700.0);
      check(cleanThd < 0.02 && drivenThd > 0.20,
            "the drive shapes at full curve and is clean at zero");
      std::printf("      THD at zero drive %.4f, at 90 %% %.3f\n", cleanThd, drivenThd);
   }

   // 8. Every drive style is finite and audibly a different shape.
   {
      bool finite = true;
      std::vector<double> thd;
      for (int style = 0; style < shapeau::kNumDriveStyles; ++style) {
         shapeau::CurveSet curves;
         shapeau::resetCurves(curves);
         shapeau::EngineParams p;
         p.drvOn = true;
         p.drvDrive = 0.8f;
         p.drvStyle = style;
         const auto out = runEngine(p, curves, sine, sine, sr);
         for (float v : out.l)
            finite = finite && std::isfinite(v) && std::fabs(v) < 8.0f;
         thd.push_back(thdAt(out.l, sr, 220.0));
      }
      bool distinct = true;
      for (size_t i = 0; i < thd.size(); ++i)
         for (size_t j = i + 1; j < thd.size(); ++j)
            if (std::fabs(thd[i] - thd[j]) < 0.01)
               distinct = false;
      check(finite, "every drive style stays finite and bounded");
      check(distinct, "every drive style produces a different amount of distortion");
   }

   // 9. The time stage at the top of the shape is the present.
   {
      shapeau::CurveSet curves;
      shapeau::resetCurves(curves);
      shapeau::EngineParams p;
      p.timOn = true;
      p.timRangeSec = 0.2f;
      p.timStep = 0.0f;
      const auto out = runEngine(p, curves, sine, sine, sr);
      const double db = 20.0 * std::log10(std::max(1.0e-9, rmsTail(out.l) / rmsTail(sine)));
      check(std::fabs(db) < 0.5, "a time shape at the top passes the present through unchanged");
      std::printf("      time at the top: %.2f dB against the input\n", db);
   }

   // 10. A falling shape reads backwards through the buffer and stays bounded.
   {
      shapeau::CurveSet curves;
      shapeau::resetCurves(curves);
      for (int b = 0; b < shapeau::kNumBands; ++b) {
         shapeau::Curve &c = curves.curve[shapeau::kModuleTime][b];
         c.setFlat(1.0f);
         c.addPoint(0.0f, 1.0f, 0);
         c.addPoint(1.0f, 0.0f, 0);
      }
      shapeau::EngineParams p;
      p.timOn = true;
      p.timRangeSec = 0.2f;
      p.timStep = 0.0f;
      p.timRate = shapeau::kRate4;
      const auto out = runEngine(p, curves, sine, sine, sr);
      bool finite = true;
      for (float v : out.l)
         finite = finite && std::isfinite(v) && std::fabs(v) < 4.0f;
      check(finite && differenceDb(sine, out.l) > -20.0,
            "a falling time shape moves the read head and stays bounded");
   }

   // 11. A band switched into silence takes only its own frequencies with it.
   {
      auto tone = [&](double hz) {
         std::vector<float> x(n);
         for (size_t i = 0; i < n; ++i)
            x[i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * hz * i / sr));
         return x;
      };
      shapeau::CurveSet curves;
      shapeau::resetCurves(curves);
      curves.curve[shapeau::kModuleVolume][shapeau::kBandLow].setFlat(0.0f);
      shapeau::EngineParams p = neutralParams();
      p.splitLowHz = 400.0f;
      p.splitHighHz = 2000.0f;
      const auto low = runEngine(p, curves, tone(100.0), tone(100.0), sr);
      const auto high = runEngine(p, curves, tone(5000.0), tone(5000.0), sr);
      const double lowDb = 20.0 * std::log10(std::max(1.0e-9, rms(low.l) / rms(tone(100.0))));
      const double highDb = 20.0 * std::log10(std::max(1.0e-9, rms(high.l) / rms(tone(5000.0))));
      check(lowDb < -20.0 && std::fabs(highDb) < 1.5,
            "silencing the low band leaves the top of the signal alone");
   }

   // 11b. Pan, width and crush: neutral curves are transparent, a hard pan
   // takes one channel away, mono width really sums, and the crusher quantises.
   {
      shapeau::CurveSet curves;
      shapeau::resetCurves(curves);
      const bool neutral =
         std::fabs(shapeau::neutralValue(shapeau::kModulePan) - 0.5f) < 1.0e-6f &&
         std::fabs(shapeau::neutralValue(shapeau::kModuleWidth) - 0.5f) < 1.0e-6f &&
         std::fabs(curves.curve[shapeau::kModulePan][0].valueAt(0.3f) - 0.5f) < 1.0e-6f &&
         std::fabs(curves.curve[shapeau::kModuleWidth][0].valueAt(0.7f) - 0.5f) < 1.0e-6f;
      check(neutral, "pan and width reset to their centre, not to the top");

      shapeau::EngineParams p;
      p.panOn = true;
      p.widOn = true;
      p.panSmooth = 0.0f;
      p.widSmooth = 0.0f;
      shapeau::EngineParams referenceParams;
      referenceParams.volOn = true; // keeps the band split in the path
      const auto reference = runEngine(referenceParams, curves, sine, sine, sr);
      const auto flat = runEngine(p, curves, sine, sine, sr);
      // Against the same rack with the stages off, not against the input: the
      // band split itself turns the phase, so a sample comparison would be
      // measuring the crossover rather than these stages.
      const double flatDb =
         20.0 * std::log10(std::max(1.0e-9, rmsTail(flat.l) / rmsTail(reference.l)));
      check(std::fabs(flatDb) < 0.01,
            "a neutral pan and width are transparent");
      std::printf("      neutral pan/width: %.3f dB against the same rack with them off\n", flatDb);

      for (int b = 0; b < shapeau::kNumBands; ++b)
         curves.curve[shapeau::kModulePan][b].setFlat(0.0f);
      const auto left = runEngine(p, curves, sine, sine, sr);
      const double leftKeep = rmsTail(left.l) / rmsTail(sine);
      const double rightKeep = rmsTail(left.r) / rmsTail(sine);
      check(leftKeep > 0.9 && rightKeep < 0.01,
            "pan at the bottom of its curve is hard left");

      shapeau::CurveSet mono;
      shapeau::resetCurves(mono);
      for (int b = 0; b < shapeau::kNumBands; ++b)
         mono.curve[shapeau::kModuleWidth][b].setFlat(0.0f);
      shapeau::EngineParams w;
      w.widOn = true;
      w.widSmooth = 0.0f;
      std::vector<float> rightZero(n, 0.0f);
      const auto narrowed = runEngine(w, mono, sine, rightZero, sr);
      bool same = true;
      for (size_t i = n / 5; i < n; ++i)
         same = same && std::fabs(narrowed.l[i] - narrowed.r[i]) < 1.0e-5f;
      check(same, "width at the bottom of its curve sums a band to mono");

      shapeau::CurveSet clean;
      shapeau::resetCurves(clean);
      shapeau::EngineParams off;
      off.crsOn = true;
      off.crsAmount = 0.0f;
      const auto cleanOut = runEngine(off, clean, tone700, tone700, sr);
      const double cleanDb =
         20.0 * std::log10(std::max(1.0e-9, rmsTail(cleanOut.l) / rmsTail(tone700)));
      check(std::fabs(cleanDb) < 0.1, "the crush stage at zero amount is clean");

      shapeau::EngineParams c = off;
      c.crsAmount = 1.0f;
      c.crsBits = 4;
      c.crsDown = 8;
      c.crsMix = 1.0f;
      c.crsSmooth = 0.0f;
      const auto crushed = runEngine(c, clean, tone700, tone700, sr);
      bool finite = true;
      for (float v : crushed.l)
         finite = finite && std::isfinite(v) && std::fabs(v) < 4.0f;
      // Bit reduction and a held sample are noise and aliases more than they
      // are harmonics, so this compares the crushed signal to the clean one
      // rather than asking the THD meter about harmonic 2-7.
      const double changed = differenceDb(cleanOut.l, crushed.l);
      check(finite && changed > -25.0,
            "the crush stage quantises and holds, and stays bounded");
      std::printf("      crush against clean: %.1f dB\n", changed);
   }

   // 12. A preset carries the shapes through its text form.
   {
      shapeau::PresetData preset;
      preset.name = "Round Trip";
      preset.values.emplace_back(shapeau::kParamDrvDrive, 0.62);
      preset.values.emplace_back(shapeau::kParamTimRange, 0.4);
      shapeau::CurveSet curves;
      shapeau::resetCurves(curves);
      curves.curve[shapeau::kModuleFilter][shapeau::kBandHigh].setFlat(1.0f);
      curves.curve[shapeau::kModuleFilter][shapeau::kBandHigh].addPoint(0.5f, 0.0f, 2);
      const std::string text = shapeau::formatPreset(preset, curves);

      shapeau::PresetData back;
      shapeau::CurveSet backCurves;
      std::string error;
      const bool parsed = shapeau::parsePreset(text.c_str(), text.size(), back, backCurves, error);
      bool same = parsed && back.name == preset.name;
      same = same && std::fabs(backCurves.curve[shapeau::kModuleFilter][shapeau::kBandHigh]
                                    .valueAt(0.5f) - 0.0f) < 1.0e-3;
      same = same && std::fabs(backCurves.curve[shapeau::kModuleFilter][shapeau::kBandLow]
                                    .valueAt(0.5f) - 1.0f) < 1.0e-3;
      check(same, "a preset's shapes survive writing and reading");
   }

   // 13. Every factory preset parses and carries all of its shapes.
   {
      int files = 0, bad = 0;
      std::error_code ec;
      for (const auto &entry : std::filesystem::directory_iterator(SHAPEAU_PRESET_DIR, ec)) {
         if (!entry.is_regular_file() || entry.path().extension() != ".shapeau")
            continue;
         ++files;
         shapeau::PresetData data;
         shapeau::CurveSet curves;
         std::string error;
         if (!shapeau::parsePresetFileWithCurves(entry.path().string(), data, curves, error)) {
            std::printf("      %s: %s\n", entry.path().filename().string().c_str(), error.c_str());
            ++bad;
         }
      }
      check(files > 0 && bad == 0, "every factory preset parses with its shapes");

      // The format stores a percentage as a ratio, 0..1. Written as 0..100 it
      // still parses -- and every value but 0 and 100 lands on the top of its
      // range, which is how all nine factory presets once loaded with every
      // Smooth, Drive and Resonance at 100 %. Each value must load as written.
      int wrong = 0;
      for (const auto &entry : std::filesystem::directory_iterator(SHAPEAU_PRESET_DIR, ec)) {
         if (!entry.is_regular_file() || entry.path().extension() != ".shapeau")
            continue;
         std::ifstream in(entry.path());
         shapeau::PresetData data;
         shapeau::CurveSet curves;
         std::string error;
         shapeau::parsePresetFileWithCurves(entry.path().string(), data, curves, error);
         std::string line;
         while (std::getline(in, line)) {
            const size_t eq = line.find(" = ");
            if (eq == std::string::npos)
               continue;
            const shapeau::ParamDesc *d = shapeau::paramByKey(line.substr(0, eq).c_str());
            if (!d || d->kind != shapeau::ParamKind::Percent)
               continue;
            const double written = std::atof(line.c_str() + eq + 3);
            for (const auto &kv : data.values)
               if (kv.first == d->id && std::fabs(kv.second - written) > 1.0e-9) {
                  std::printf("      %s: %s\n", entry.path().filename().string().c_str(),
                              line.c_str());
                  ++wrong;
               }
         }
      }
      check(files > 0 && wrong == 0, "every factory preset's percentages load as written");
   }

   // 14. The chain's order is the audio's order. A volume duck into a hard
   //     drive and the same drive into the duck are different signals: the
   //     drive ahead of the duck clips a full-level signal, and after it the
   //     clipper sees the ducked one. With an empty list the effects that are on
   //     run in the old order, which is what every preset written before the
   //     chain existed relies on.
   {
      shapeau::CurveSet curves;
      shapeau::resetCurves(curves);
      curves.curve[shapeau::kModuleVolume][shapeau::kBandMid].setFlat(1.0f);
      for (int b = 0; b < shapeau::kNumBands; ++b) {
         curves.curve[shapeau::kModuleVolume][b].setFlat(0.3f);
      }
      shapeau::EngineParams p = neutralParams();
      p.drvOn = true;
      p.drvDrive = 1.0f;
      p.drvStyle = shapeau::kHardClip;
      p.drvSmooth = 0.0f;
      const auto unlisted = runEngine(p, curves, sine, sine, sr);
      shapeau::chainAdd(curves, shapeau::kModuleVolume);
      shapeau::chainAdd(curves, shapeau::kModuleDrive);
      const auto volFirst = runEngine(p, curves, sine, sine, sr);
      shapeau::chainMove(curves, shapeau::kModuleDrive, 0);
      const auto drvFirst = runEngine(p, curves, sine, sine, sr);
      check(differenceDb(unlisted.l, volFirst.l) < -120.0,
            "an empty chain runs what is switched on in the old order");
      check(differenceDb(volFirst.l, drvFirst.l) > -30.0,
            "moving an effect in the chain changes what comes out");
      std::printf("      drive first against volume first: %.1f dB\n",
                  differenceDb(volFirst.l, drvFirst.l));

      shapeau::CurveSet back;
      shapeau::PresetData preset, presetBack;
      preset.name = "Chain";
      const std::string text = shapeau::formatPreset(preset, curves);
      std::string error;
      check(shapeau::parsePreset(text.c_str(), text.size(), presetBack, back, error) &&
               back.chainCount == 2 && back.chain[0] == shapeau::kModuleDrive &&
               back.chain[1] == shapeau::kModuleVolume,
            "a preset's chain survives writing and reading, in order");
   }

   // 15. Smooth acts on Drive and Filter. A shape that jumps is followed at
   //     once with Smooth at zero and glided with it at the top, so the two
   //     renders part; both used to come out the same, because the setting
   //     was read and never applied.
   {
      shapeau::CurveSet curves;
      shapeau::resetCurves(curves);
      for (int b = 0; b < shapeau::kNumBands; ++b) {
         curves.curve[shapeau::kModuleDrive][b].setStep(0, 2, 0.0f);
         curves.curve[shapeau::kModuleDrive][b].setStep(1, 2, 1.0f);
         curves.curve[shapeau::kModuleFilter][b].setStep(0, 2, 0.0f);
         curves.curve[shapeau::kModuleFilter][b].setStep(1, 2, 1.0f);
      }
      shapeau::EngineParams p = neutralParams();
      p.volOn = false;
      p.drvOn = true;
      p.drvDrive = 0.9f;
      p.drvSmooth = 0.0f;
      const auto drvHard = runEngine(p, curves, sine, sine, sr);
      p.drvSmooth = 1.0f;
      const auto drvSoft = runEngine(p, curves, sine, sine, sr);
      check(differenceDb(drvHard.l, drvSoft.l) > -40.0, "Drive's Smooth glides the drive amount");

      p.drvOn = false;
      p.fltOn = true;
      p.fltCutHz = 2000.0f;
      p.fltSmooth = 0.0f;
      const auto fltHard = runEngine(p, curves, noise, noise, sr);
      p.fltSmooth = 1.0f;
      const auto fltSoft = runEngine(p, curves, noise, noise, sr);
      check(differenceDb(fltHard.l, fltSoft.l) > -40.0, "Filter's Smooth glides the cutoff");
   }

   // 16. The scope is written along the selected effect's cycle. A click on
   //     every beat with a quarter-note shape lands in the first bins of the
   //     input picture and nowhere near the middle; the playhead is a phase;
   //     and with the editor closed nothing is written at all.
   {
      // Four beats: the test signal everything else uses is half a second,
      // which at 120 BPM is a single beat.
      const size_t beat = static_cast<size_t>(sr * 60.0 / 120.0);
      const size_t len = 4 * beat;
      std::vector<float> clicks(len, 0.0f);
      for (size_t i = 0; i < len; i += beat)
         clicks[i] = 0.9f;
      shapeau::CurveSet curves;
      shapeau::resetCurves(curves);
      shapeau::EngineParams p = neutralParams();
      shapeau::Engine engine;
      engine.prepare(sr, 512);
      engine.setParams(p);
      engine.publishCurves(curves);
      std::vector<float> l = clicks, r = clicks;
      engine.process(l.data(), r.data(), 256, 0.0, 120.0);
      check(engine.scope().blocks.load() == 0, "the scope writes nothing while no editor is open");
      engine.setScopeTarget(shapeau::kModuleVolume, shapeau::kBandMid, true, true);
      double beats = 256 * 2.0 / sr;
      for (size_t pos = 256; pos + 256 <= len; pos += 256) {
         engine.process(l.data() + pos, r.data() + pos, 256, beats, 120.0);
         beats += 256 * 2.0 / sr;
      }
      const shapeau::Scope &scope = engine.scope();
      const int bins = shapeau::Scope::kBins;
      float early = 0.0f, middle = 0.0f;
      for (int i = 0; i < bins / 20; ++i)
         early = std::max(early, scope.inHi[i].load());
      for (int i = bins / 3; i < 2 * bins / 3; ++i)
         middle = std::max(middle, scope.inHi[i].load());
      const float head = scope.playhead.load();
      check(early > 0.8f && middle < 0.01f && head >= 0.0f && head < 1.0f,
            "the scope puts a beat's click at the start of a quarter-note cycle");
      float trace = -1.0f;
      for (int i = 0; i < bins; ++i)
         trace = std::max(trace, scope.trace[i].load());
      check(std::fabs(trace - 1.0f) < 1.0e-3f, "and traces a flat volume shape at the top");
   }

   // 17. Triggers. A volume shape that is silent for its first half and open
   //     for its second, a quarter note long at 120 BPM (24000 samples), on a
   //     steady 0.5 sine. In MIDI or Audio mode it rests at its End until
   //     something starts it, so nothing is ducked before the first trigger.
   {
      const size_t cycle = static_cast<size_t>(sr * 60.0 / 120.0);
      const size_t len = 6 * cycle;
      std::vector<float> tone(len);
      for (size_t i = 0; i < len; ++i)
         tone[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 440.0 * i / sr));
      // A kick: 60 Hz falling from 150, gone in about 100 ms.
      auto kickAt = [&](std::vector<float> &into, size_t at, float peak) {
         double phase = 0.0;
         for (size_t i = 0; at + i < into.size() && i < static_cast<size_t>(sr * 0.25); ++i) {
            const double t = i / sr;
            phase += 2.0 * kPi * (60.0 + 90.0 * std::exp(-t * 40.0)) / sr;
            into[at + i] += static_cast<float>(peak * std::exp(-t * 25.0) * std::sin(phase));
         }
      };
      shapeau::CurveSet curves;
      shapeau::resetCurves(curves);
      for (int b = 0; b < shapeau::kNumBands; ++b) {
         curves.curve[shapeau::kModuleVolume][b].setStep(0, 2, 0.0f);
         curves.curve[shapeau::kModuleVolume][b].setStep(1, 2, 1.0f);
      }
      shapeau::EngineParams base = neutralParams();
      base.volSmooth = 0.0f;

      struct Trig {
         Run out;
         uint32_t fired = 0;
      };
      // Runs the engine with a sidechain and MIDI trigger frames (absolute),
      // the transport playing from zero at 120 BPM.
      auto run = [&](const shapeau::EngineParams &p, const std::vector<float> &in,
                     const std::vector<float> *side, std::vector<size_t> notes,
                     bool scope = false) {
         shapeau::Engine engine;
         engine.prepare(sr, 512);
         engine.setParams(p);
         engine.publishCurves(curves);
         if (scope)
            engine.setScopeTarget(shapeau::kModuleVolume, shapeau::kBandMid, true, true);
         Trig t;
         t.out.l = in;
         t.out.r = in;
         constexpr uint32_t kBlock = 256;
         double beats = 0.0;
         for (size_t pos = 0; pos < len; pos += kBlock) {
            const uint32_t n = static_cast<uint32_t>(std::min<size_t>(kBlock, len - pos));
            uint32_t frames[8];
            uint32_t count = 0;
            for (size_t at : notes)
               if (at >= pos && at < pos + n && count < 8)
                  frames[count++] = static_cast<uint32_t>(at - pos);
            const float *sl = side ? side->data() + pos : nullptr;
            engine.process(t.out.l.data() + pos, t.out.r.data() + pos, n, beats, 120.0, sl, sl,
                           frames, count);
            beats += n * 2.0 / sr;
         }
         t.fired = engine.scope().triggers.load();
         if (scope) {
            const shapeau::Scope &sc = engine.scope();
            float early = 0.0f, middle = 0.0f;
            for (int i = 0; i < shapeau::Scope::kBins / 40; ++i)
               early = std::max(early, sc.sideHi[i].load());
            for (int i = shapeau::Scope::kBins / 2 + 2; i < shapeau::Scope::kBins; ++i)
               middle = std::max(middle, sc.sideHi[i].load());
            t.fired = early > 0.4f && middle < 0.01f ? 1u : 0u;
         }
         return t;
      };
      auto level = [&](const Run &r, size_t from, size_t to) {
         float peak = 0.0f;
         for (size_t i = from; i < to && i < r.l.size(); ++i)
            peak = std::max(peak, std::fabs(r.l[i]));
         return peak;
      };

      shapeau::EngineParams p = base;
      p.trigMode[shapeau::kModuleVolume] = shapeau::kTrigMidi;
      const auto idle = run(p, tone, nullptr, {});
      check(level(idle.out, 2000, len) > 0.45f,
            "a triggered shape rests at its End and ducks nothing before a trigger");

      const size_t note = 2 * cycle + 1000;
      const auto once = run(p, tone, nullptr, {note});
      check(level(once.out, 2000, note) > 0.45f && level(once.out, note + 64, note + cycle / 2 - 64) < 0.005f &&
               level(once.out, note + cycle / 2 + 300, note + cycle) > 0.45f &&
               level(once.out, note + cycle + 300, len) > 0.45f,
            "a MIDI note starts the shape once: silent half a cycle, then open and held");

      p.trigLoop[shapeau::kModuleVolume] = true;
      const auto loop = run(p, tone, nullptr, {note});
      check(level(loop.out, note + cycle + 64, note + cycle + cycle / 2 - 64) < 0.005f &&
               level(loop.out, note + 2 * cycle + 64, note + 2 * cycle + cycle / 2 - 64) < 0.005f,
            "with Loop on, a triggered shape keeps cycling from the note");

      p.trigLoop[shapeau::kModuleVolume] = false;
      p.trigEnd[shapeau::kModuleVolume] = 0.25f;
      const auto held = run(p, tone, nullptr, {note});
      check(level(held.out, note + 64, len) < 0.005f,
            "a shape run once holds at its End marker until the next trigger");
      p.trigEnd[shapeau::kModuleVolume] = 1.0f;

      // Audio: two kicks in the sidechain, one cycle and a half apart.
      std::vector<float> side(len, 0.0f);
      const size_t kick1 = cycle, kick2 = cycle + cycle * 3 / 2;
      kickAt(side, kick1, 0.8f);
      kickAt(side, kick2, 0.8f);
      p.trigMode[shapeau::kModuleVolume] = shapeau::kTrigAudio;
      const auto audio = run(p, tone, &side, {});
      check(audio.fired == 2, "the detector finds both kicks in the sidechain, once each");
      // A few samples to cross the threshold, one 16-sample control block, and
      // the volume stage's shortest smoothing (0.3 ms) falling to 1 %.
      check(level(audio.out, 2000, kick1) > 0.45f &&
               level(audio.out, kick1 + 128, kick1 + cycle / 2 - 64) < 0.005f &&
               level(audio.out, kick1 + cycle / 2 + 300, kick2) > 0.45f &&
               level(audio.out, kick2 + 128, kick2 + cycle / 2 - 64) < 0.005f,
            "a kick in the sidechain has the duck down within 3 ms of its onset");
      std::printf("      %u triggers from two kicks\n", audio.fired);

      std::vector<float> quiet(len, 0.0f);
      kickAt(quiet, kick1, 0.03f); // -30 dB, under the -24 dB threshold
      p.trigThreshold = std::pow(10.0f, -24.0f / 20.0f);
      const auto under = run(p, tone, &quiet, {});
      check(under.fired == 0 && level(under.out, 2000, len) > 0.45f,
            "a hit under the threshold triggers nothing");

      // Two short hits 60 ms apart: Hold decides whether that is one trigger
      // or two.
      std::vector<float> pair(len, 0.0f);
      for (size_t k = 0; k < 2; ++k)
         for (size_t i = 0; i < static_cast<size_t>(sr * 0.004); ++i)
            pair[kick1 + k * static_cast<size_t>(sr * 0.06) + i] =
               static_cast<float>(0.1 * std::sin(2.0 * kPi * 100.0 * i / sr));
      p.trigHoldSec = 0.1f;
      const auto longHold = run(p, tone, &pair, {});
      p.trigHoldSec = 0.02f;
      const auto shortHold = run(p, tone, &pair, {});
      check(longHold.fired == 1 && shortHold.fired == 2,
            "Hold merges hits closer than it into one trigger");
      p.trigHoldSec = 0.05f;

      // The detector's source: the input instead of the sidechain. The 440 Hz
      // tone under the kick is louder than the threshold on its own; Focus at
      // 150 Hz takes it below, which is what Focus is for.
      std::vector<float> withKick = tone;
      kickAt(withKick, kick1, 0.8f);
      p.trigFromInput = true;
      p.trigThreshold = std::pow(10.0f, -12.0f / 20.0f);
      const auto unfocused = run(p, withKick, nullptr, {});
      p.trigFilterHz = 150.0f;
      const auto fromInput = run(p, withKick, nullptr, {});
      // The lowpass delays the detector a little: 256 samples is about 5 ms.
      check(fromInput.fired == 1 &&
               level(fromInput.out, 2000, kick1) > 0.45f &&
               level(fromInput.out, kick1 + 256, kick1 + cycle / 2 - 64) < 0.005f,
            "with Source at Input and Focus at 150 Hz, the kick in the signal starts the duck");
      check(unfocused.fired == 1 && level(unfocused.out, 2000, cycle / 2 - 64) < 0.005f,
            "and without Focus, the tone under it fires first, at the start");
      p.trigFromInput = false;
      p.trigFilterHz = 20000.0f;
      p.trigThreshold = std::pow(10.0f, -24.0f / 20.0f);

      // The sidechain is listened to, never heard: in Sync mode the output is
      // the same with and without it.
      const auto plain = runEngine(base, curves, tone, tone, sr, 120.0);
      const auto withSide = run(base, tone, &side, {});
      check(differenceDb(plain.l, withSide.out.l) < -120.0,
            "the sidechain is never heard");

      // With the scope open on a triggered effect, the kick that started the
      // cycle is drawn at its start: that is what lines a duck up with a kick.
      const auto drawn = run(p, tone, &side, {}, true);
      check(drawn.fired == 1, "the scope draws the sidechain's kick at the start of its cycle");

      // Kicks every 0.78 of a cycle restart it before it ends, so the last
      // fifth of the picture is never reached and must stay empty -- not be
      // filled with the sample at each restart, as if the jump back to the
      // start were a short wrap forwards.
      {
         std::vector<float> early(len, 0.0f);
         for (size_t at = cycle; at < len; at += cycle * 78 / 100)
            kickAt(early, at, 0.8f);
         shapeau::Engine engine;
         engine.prepare(sr, 512);
         engine.setParams(p);
         engine.publishCurves(curves);
         engine.setScopeTarget(shapeau::kModuleVolume, shapeau::kBandMid, true, true);
         std::vector<float> l = tone, r = tone;
         double beats = 0.0;
         for (size_t pos = 0; pos + 256 <= len; pos += 256) {
            engine.process(l.data() + pos, r.data() + pos, 256, beats, 120.0,
                           early.data() + pos, early.data() + pos);
            beats += 256 * 2.0 / sr;
         }
         float tail = 0.0f;
         for (int i = shapeau::Scope::kBins * 82 / 100; i < shapeau::Scope::kBins; ++i)
            tail = std::max({tail, std::fabs(engine.scope().inHi[i].load()),
                             std::fabs(engine.scope().inLo[i].load())});
         check(tail == 0.0f, "a trigger before the end of the cycle leaves the scope's end unwritten");
      }
   }
}

// ---------------------------------------------------------------- the CLAP host

struct HostCtx {
   int rescans = 0;
   std::string lastPresetError;
};

HostCtx gCtx;

const clap_host_log_t kHostLog = {[](const clap_host_t *, clap_log_severity, const char *) {}};
const clap_host_params_t kHostParams = {
   [](const clap_host_t *, clap_param_rescan_flags) { ++gCtx.rescans; },
   [](const clap_host_t *, clap_id, clap_param_clear_flags) {},
   [](const clap_host_t *) {},
};
const clap_host_thread_check_t kHostThreadCheck = {
   [](const clap_host_t *) { return true; },
   [](const clap_host_t *) { return false; },
};
const clap_host_preset_load_t kHostPresetLoad = {
   [](const clap_host_t *, uint32_t, const char *, const char *, int32_t, const char *msg) {
      gCtx.lastPresetError = msg ? msg : "";
   },
   [](const clap_host_t *, uint32_t, const char *, const char *) {},
};

clap_host_t gHost = {
   CLAP_VERSION_INIT,
   &gCtx,
   "shapeau-render",
   "Ravetracer",
   "https://github.com/Ravetracer/audio-plugins",
   "0.1.0",
   [](const clap_host_t *, const char *id) -> const void * {
      if (std::strcmp(id, CLAP_EXT_LOG) == 0)
         return &kHostLog;
      if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
         return &kHostParams;
      if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0)
         return &kHostThreadCheck;
      if (std::strcmp(id, CLAP_EXT_PRESET_LOAD) == 0)
         return &kHostPresetLoad;
      return nullptr;
   },
   [](const clap_host_t *) {},
   [](const clap_host_t *) {},
   [](const clap_host_t *) {},
};

struct Plugin {
   void *handle = nullptr;
   const clap_plugin_entry_t *entry = nullptr;
   const clap_plugin_factory_t *factory = nullptr;
   const clap_plugin_t *plugin = nullptr;

   ~Plugin() {
      if (plugin)
         plugin->destroy(plugin);
      if (entry && entry->deinit)
         entry->deinit();
      if (handle)
         RD_DLCLOSE(handle);
   }
};

bool loadPlugin(Plugin &out, const std::string &path) {
   out.handle = RD_DLOPEN(path.c_str());
   if (!out.handle) {
      std::fprintf(stderr, "cannot load %s\n", path.c_str());
      return false;
   }
   out.entry = static_cast<const clap_plugin_entry_t *>(RD_DLSYM(out.handle, "clap_entry"));
   if (!out.entry || !out.entry->init(path.c_str())) {
      std::fprintf(stderr, "%s is not a CLAP plugin\n", path.c_str());
      return false;
   }
   out.factory =
      static_cast<const clap_plugin_factory_t *>(out.entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
   if (!out.factory) {
      std::fprintf(stderr, "%s has no plugin factory\n", path.c_str());
      return false;
   }
   return true;
}

// Deques, not vectors: the headers point into the stored events, and a vector
// that grows moves them.
struct EventList {
   std::deque<clap_event_param_value_t> values;
   std::deque<clap_event_note_t> notes;
   std::vector<const clap_event_header_t *> headers;
   void add(clap_event_param_value_t v) {
      values.push_back(v);
      headers.push_back(&values.back().header);
   }
   void addNote(uint32_t time, int16_t key) {
      clap_event_note_t n{};
      n.header.size = sizeof(n);
      n.header.type = CLAP_EVENT_NOTE_ON;
      n.header.time = time;
      n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      n.note_id = -1;
      n.port_index = 0;
      n.channel = 0;
      n.key = key;
      n.velocity = 1.0;
      notes.push_back(n);
      headers.push_back(&notes.back().header);
   }
   void clear() {
      values.clear();
      notes.clear();
      headers.clear();
   }
   static uint32_t size(const clap_input_events_t *list) {
      return static_cast<uint32_t>(static_cast<const EventList *>(list->ctx)->headers.size());
   }
   static const clap_event_header_t *get(const clap_input_events_t *list, uint32_t index) {
      const auto *self = static_cast<const EventList *>(list->ctx);
      return index < self->headers.size() ? self->headers[index] : nullptr;
   }
};

struct OutEvents {
   static bool push(const clap_output_events_t *, const clap_event_header_t *) { return true; }
};

bool exercisePlugin(const std::string &path) {
   Plugin p;
   if (!loadPlugin(p, path)) {
      check(false, "the built plugin loads as a CLAP module");
      return false;
   }
   check(true, "the built plugin loads as a CLAP module");

   const uint32_t count = p.factory->get_plugin_count(p.factory);
   const clap_plugin_descriptor_t *desc = count ? p.factory->get_plugin_descriptor(p.factory, 0) : nullptr;
   check(count == 1 && desc && std::strcmp(desc->id, shapeau::kPluginId) == 0 &&
            std::strcmp(desc->version, shapeau::kPluginVersion) == 0,
         "the factory describes one plugin with Shapeau's id and version");

   p.plugin = p.factory->create_plugin(p.factory, &gHost, shapeau::kPluginId);
   check(p.plugin != nullptr, "the plugin instantiates");
   if (!p.plugin)
      return false;
   p.plugin->init(p.plugin);

   const auto *params = static_cast<const clap_plugin_params_t *>(
      p.plugin->get_extension(p.plugin, CLAP_EXT_PARAMS));
   check(params && params->count(p.plugin) == shapeau::kNumParams,
         "the plugin exposes the whole parameter table");

   const auto *ports = static_cast<const clap_plugin_audio_ports_t *>(
      p.plugin->get_extension(p.plugin, CLAP_EXT_AUDIO_PORTS));
   check(ports && ports->count(p.plugin, true) == 2 && ports->count(p.plugin, false) == 1,
         "the plugin has a stereo input, a sidechain input and a stereo output");
   {
      clap_audio_port_info_t side{};
      const bool ok = ports && ports->get(p.plugin, 1, true, &side);
      check(ok && side.channel_count == 2 && !(side.flags & CLAP_AUDIO_PORT_IS_MAIN) &&
               std::strcmp(side.name, "Sidechain") == 0,
            "the second input is a stereo sidechain that is not main");
   }
   const auto *notePorts = static_cast<const clap_plugin_note_ports_t *>(
      p.plugin->get_extension(p.plugin, CLAP_EXT_NOTE_PORTS));
   check(notePorts && notePorts->count(p.plugin, true) == 1 &&
            notePorts->count(p.plugin, false) == 0,
         "the plugin takes notes in, for the MIDI trigger");

   p.plugin->activate(p.plugin, 48000.0, 1, 512);
   p.plugin->start_processing(p.plugin);

   float left[256] = {0}, right[256] = {0};
   clap_audio_buffer_t inBuf{};
   clap_audio_buffer_t outBuf{};
   float *inPtrs[2] = {left, right};
   float *outPtrs[2] = {left, right};
   inBuf.data32 = inPtrs;
   inBuf.channel_count = 2;
   outBuf.data32 = outPtrs;
   outBuf.channel_count = 2;
   EventList events;
   clap_input_events_t inEvents{&events, EventList::size, EventList::get};
   OutEvents outStorage;
   clap_output_events_t outEvents{&outStorage, OutEvents::push};

   clap_event_transport_t transport{};
   transport.header.size = sizeof(transport);
   transport.header.type = CLAP_EVENT_TRANSPORT;
   transport.flags = CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_IS_PLAYING |
                     CLAP_TRANSPORT_HAS_TEMPO;
   transport.tempo = 130.0;
   transport.song_pos_beats = 0;

   clap_process_t proc{};
   proc.frames_count = 256;
   proc.transport = &transport;
   proc.audio_inputs = &inBuf;
   proc.audio_inputs_count = 1;
   proc.audio_outputs = &outBuf;
   proc.audio_outputs_count = 1;
   proc.in_events = &inEvents;
   proc.out_events = &outEvents;

   bool finite = true;
   for (int block = 0; block < 16; ++block) {
      for (int i = 0; i < 256; ++i) {
         left[i] = static_cast<float>(0.2 * std::sin(2.0 * kPi * 220.0 * (block * 256 + i) / 48000.0));
         right[i] = left[i];
      }
      transport.song_pos_beats = static_cast<int64_t>(block) * 256 * CLAP_BEATTIME_FACTOR *
                                 130 / (60 * 48000);
      p.plugin->process(p.plugin, &proc);
      for (int i = 0; i < 256; ++i)
         finite = finite && std::isfinite(left[i]) && std::isfinite(right[i]);
   }
   check(finite, "the plugin processes audio without producing anything non-finite");

   // Preset load through the host-facing extension, one file, then the first
   // built-in by its load key.
   const auto *presetLoad = static_cast<const clap_plugin_preset_load_t *>(
      p.plugin->get_extension(p.plugin, CLAP_EXT_PRESET_LOAD));
   check(presetLoad != nullptr, "the plugin offers CLAP preset load");

   std::string firstPreset;
   std::error_code ec;
   for (const auto &entry : std::filesystem::directory_iterator(SHAPEAU_PRESET_DIR, ec))
      if (entry.is_regular_file() && entry.path().extension() == ".shapeau") {
         firstPreset = entry.path().string();
         break;
      }
   if (presetLoad && !firstPreset.empty()) {
      const bool ok = presetLoad->from_location(p.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                                firstPreset.c_str(), nullptr);
      check(ok, "a factory preset loads through the preset-load extension");
   } else {
      check(false, "a factory preset loads through the preset-load extension");
   }

   // Kick Duck with its volume set to MIDI and then to Audio: a note event, and
   // then a kick in the sidechain buffer, each duck a 60 Hz tone that nothing
   // ducks while the volume waits for a trigger.
   std::string duck = std::string(SHAPEAU_PRESET_DIR) + "/kick_duck.shapeau";
   if (presetLoad &&
       presetLoad->from_location(p.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, duck.c_str(),
                                 nullptr)) {
      float sideL[256] = {0}, sideR[256] = {0};
      float *sidePtrs[2] = {sideL, sideR};
      clap_audio_buffer_t ins[2] = {inBuf, {}};
      ins[1].data32 = sidePtrs;
      ins[1].channel_count = 2;
      proc.audio_inputs = ins;
      proc.audio_inputs_count = 2;
      int64_t frame = 16 * 256;
      // Peak of the left output per block, `blocks` blocks long; a note or a
      // kick is put at the top of block `at`.
      auto play = [&](int blocks, int at, bool note, bool kick) {
         std::vector<float> peaks;
         for (int block = 0; block < blocks; ++block) {
            events.clear();
            if (note && block == at)
               events.addNote(0, 36);
            for (int i = 0; i < 256; ++i, ++frame) {
               left[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 60.0 * frame / 48000.0));
               right[i] = left[i];
               const int k = (block - at) * 256 + i;
               sideL[i] = sideR[i] =
                  kick && k >= 0 && k < 4800
                     ? static_cast<float>(0.8 * std::exp(-k / 1200.0) *
                                          std::sin(2.0 * kPi * 60.0 * k / 48000.0))
                     : 0.0f;
            }
            transport.song_pos_beats =
               static_cast<int64_t>(static_cast<double>(frame) * CLAP_BEATTIME_FACTOR * 130.0 /
                                    (60.0 * 48000.0));
            p.plugin->process(p.plugin, &proc);
            float peak = 0.0f;
            for (int i = 0; i < 256; ++i)
               peak = std::max(peak, std::fabs(left[i]));
            peaks.push_back(peak);
         }
         events.clear();
         return peaks;
      };
      auto setParam = [&](uint32_t id, double value) {
         clap_event_param_value_t v{};
         v.header.size = sizeof(v);
         v.header.type = CLAP_EVENT_PARAM_VALUE;
         v.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
         v.param_id = id;
         v.note_id = -1;
         v.port_index = -1;
         v.channel = -1;
         v.key = -1;
         v.value = value;
         events.add(v);
         params->flush(p.plugin, &inEvents, &outEvents);
         events.clear();
      };
      setParam(shapeau::kParamVolTrigger, shapeau::kTrigMidi);
      auto midi = play(40, 20, true, false);
      const float before = *std::min_element(midi.begin() + 8, midi.begin() + 20);
      const float after = *std::min_element(midi.begin() + 21, midi.begin() + 24);
      check(before > 0.4f && after < 0.25f * before,
            "through the CLAP API, a note-on ducks Kick Duck set to MIDI");
      std::printf("      before the note %.3f, after it %.3f\n", before, after);

      setParam(shapeau::kParamVolTrigger, shapeau::kTrigAudio);
      auto audio = play(80, 60, false, true);
      const float waiting = *std::min_element(audio.begin() + 40, audio.begin() + 60);
      const float ducked = *std::min_element(audio.begin() + 61, audio.begin() + 64);
      check(waiting > 0.4f && ducked < 0.25f * waiting,
            "and a kick in the sidechain buffer ducks it set to Audio");
      std::printf("      before the kick %.3f, after it %.3f\n", waiting, ducked);
      proc.audio_inputs = &inBuf;
      proc.audio_inputs_count = 1;
   } else {
      check(false, "Kick Duck loads for the trigger test");
   }

   p.plugin->stop_processing(p.plugin);
   p.plugin->deactivate(p.plugin);
   return true;
}

// ---------------------------------------------------------------- rendering

bool renderPreset(const std::string &pluginPath, const std::string &presetPath,
                  const std::string &outPath, double seconds, double tail, double sr,
                  const std::vector<std::pair<std::string, std::string>> &overrides) {
   Plugin p;
   if (!loadPlugin(p, pluginPath))
      return false;
   p.plugin = p.factory->create_plugin(p.factory, &gHost, shapeau::kPluginId);
   if (!p.plugin)
      return false;
   p.plugin->init(p.plugin);
   p.plugin->activate(p.plugin, sr, 1, 512);
   p.plugin->start_processing(p.plugin);

   const auto *presetLoad = static_cast<const clap_plugin_preset_load_t *>(
      p.plugin->get_extension(p.plugin, CLAP_EXT_PRESET_LOAD));
   if (!presetPath.empty()) {
      if (!presetLoad ||
          !presetLoad->from_location(p.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                     presetPath.c_str(), nullptr)) {
         std::fprintf(stderr, "cannot load preset %s: %s\n", presetPath.c_str(),
                      gCtx.lastPresetError.c_str());
         return false;
      }
   }

   EventList events;
   std::vector<clap_event_param_value_t> storage;
   for (const auto &kv : overrides) {
      const shapeau::ParamDesc *d = shapeau::paramByKey(kv.first.c_str());
      double raw = 0.0;
      // Nothing here is stochastic, but shared/tools/make-demos.sh pins the
      // seed of every plugin it renders, so the key it uses has to be accepted
      // and ignored rather than failing a demo that is otherwise fine.
      if (!d && kv.first == "randomseed")
         continue;
      if (!d || !shapeau::paramTextToValue(*d, kv.second.c_str(), &raw)) {
         std::fprintf(stderr, "no parameter '%s' takes '%s'\n", kv.first.c_str(),
                      kv.second.c_str());
         return false;
      }
      clap_event_param_value_t v{};
      v.header.size = sizeof(v);
      v.header.type = CLAP_EVENT_PARAM_VALUE;
      v.note_id = -1;
      v.port_index = -1;
      v.channel = -1;
      v.key = -1;
      v.param_id = d->id;
      v.value = raw;
      storage.push_back(v);
   }
   for (auto &v : storage)
      events.add(v);

   clap_input_events_t inEvents{&events, EventList::size, EventList::get};
   OutEvents outStorage;
   clap_output_events_t outEvents{&outStorage, OutEvents::push};

   const uint32_t block = 512;
   const size_t total = static_cast<size_t>((seconds + tail) * sr);
   std::vector<float> left(total), right(total), inL(block), inR(block);
   std::vector<float> outL(total), outR(total);

   clap_event_transport_t transport{};
   transport.header.size = sizeof(transport);
   transport.header.type = CLAP_EVENT_TRANSPORT;
   transport.flags = CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_IS_PLAYING |
                     CLAP_TRANSPORT_HAS_TEMPO;
   transport.tempo = 130.0;

   for (size_t pos = 0; pos < total; pos += block) {
      const uint32_t n = static_cast<uint32_t>(std::min<size_t>(block, total - pos));
      for (uint32_t i = 0; i < n; ++i) {
         const double t = static_cast<double>(pos + i) / sr;
         // A short, quiet chord rather than a sine: a shape that ducks, filters
         // or stutters is audible against something with an envelope, and the
         // demo renders are what this is for.
         const double s = 0.16 * std::sin(2.0 * kPi * 110.0 * t) +
                          0.10 * std::sin(2.0 * kPi * 165.0 * t) +
                          0.06 * std::sin(2.0 * kPi * 220.0 * t);
         inL[i] = static_cast<float>(s);
         inR[i] = static_cast<float>(s * 0.9);
      }
      float *inPtrs[2] = {inL.data(), inR.data()};
      float *outPtrs[2] = {outL.data() + pos, outR.data() + pos};
      clap_audio_buffer_t inBuf{};
      inBuf.data32 = inPtrs;
      inBuf.channel_count = 2;
      clap_audio_buffer_t outBuf{};
      outBuf.data32 = outPtrs;
      outBuf.channel_count = 2;

      clap_process_t proc{};
      proc.frames_count = n;
      proc.transport = &transport;
      proc.audio_inputs = &inBuf;
      proc.audio_inputs_count = 1;
      proc.audio_outputs = &outBuf;
      proc.audio_outputs_count = 1;
      proc.in_events = &inEvents;
      proc.out_events = &outEvents;
      transport.song_pos_beats = static_cast<int64_t>(
         static_cast<double>(pos) * (130.0 / 60.0) / sr * static_cast<double>(CLAP_BEATTIME_FACTOR));
      p.plugin->process(p.plugin, &proc);

      // Parameter overrides belong to the first block only; after that the
      // host would be re-sending them every block.
      events.clear();
   }

   p.plugin->stop_processing(p.plugin);
   p.plugin->deactivate(p.plugin);

   if (!writeWav(outPath, outL, outR, sr)) {
      std::fprintf(stderr, "cannot write %s\n", outPath.c_str());
      return false;
   }
   std::printf("%s\n", outPath.c_str());
   return true;
}

void listPresets() {
   std::error_code ec;
   int count = 0;
   for (const auto &entry : std::filesystem::directory_iterator(SHAPEAU_PRESET_DIR, ec)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".shapeau")
         continue;
      shapeau::PresetData data;
      shapeau::CurveSet curves;
      std::string error;
      if (shapeau::parsePresetFileWithCurves(entry.path().string(), data, curves, error)) {
         std::printf("%-28s %s\n", data.name.c_str(), data.description.c_str());
         ++count;
      }
   }
   std::printf("%d presets\n", count);
}

} // namespace

int main(int argc, char **argv) {
   std::string plugin = "build/Shapeau.clap";
   std::string preset, out, outdir;
   double seconds = 4.0, tail = 1.0, rate = 48000.0;
   bool selftest = false, all = false, list = false;
   std::vector<std::pair<std::string, std::string>> overrides;

   for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
      if (a == "--plugin")
         plugin = next();
      else if (a == "--preset")
         preset = next();
      else if (a == "--out")
         out = next();
      else if (a == "--outdir")
         outdir = next();
      else if (a == "--seconds")
         seconds = std::atof(next().c_str());
      else if (a == "--tail")
         tail = std::atof(next().c_str());
      else if (a == "--rate")
         rate = std::atof(next().c_str());
      else if (a == "--param") {
         const std::string kv = next();
         const size_t eq = kv.find('=');
         if (eq == std::string::npos) {
            std::fprintf(stderr, "--param takes key=value\n");
            return 2;
         }
         overrides.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
      } else if (a == "--selftest")
         selftest = true;
      else if (a == "--all")
         all = true;
      else if (a == "--list")
         list = true;
      else {
         std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
         return 2;
      }
   }

   if (list) {
      listPresets();
      return 0;
   }

   if (selftest) {
      selftestEngine();
      exercisePlugin(plugin);
      std::printf("\n%d checks, %d failed\n", gPass + gFail, gFail);
      return gFail == 0 ? 0 : 1;
   }

   if (all) {
      if (outdir.empty()) {
         std::fprintf(stderr, "--all needs --outdir\n");
         return 2;
      }
      std::filesystem::create_directories(outdir);
      std::error_code ec;
      int made = 0, failed = 0;
      for (const auto &entry : std::filesystem::directory_iterator(SHAPEAU_PRESET_DIR, ec)) {
         if (!entry.is_regular_file() || entry.path().extension() != ".shapeau")
            continue;
         const std::string name = entry.path().stem().string();
         const std::string target = outdir + "/" + name + ".wav";
         if (renderPreset(plugin, entry.path().string(), target, seconds, tail, rate, overrides))
            ++made;
         else
            ++failed;
      }
      std::printf("%d rendered, %d failed\n", made, failed);
      return failed == 0 ? 0 : 1;
   }

   if (out.empty()) {
      std::fprintf(stderr,
                   "usage: shapeau-render --selftest [--plugin path]\n"
                   "       shapeau-render --plugin path --preset file --out file.wav\n"
                   "       shapeau-render --plugin path --all --outdir dir\n"
                   "       shapeau-render --list\n");
      return 2;
   }
   return renderPreset(plugin, preset, out, seconds, tail, rate, overrides) ? 0 : 1;
}
