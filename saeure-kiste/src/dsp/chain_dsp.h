#pragma once

// The building blocks the effect chain is made of: a state-variable filter, a
// biquad, one-pole filters, Linkwitz-Riley crossovers, an oversampler and the
// delay lines the time effects run on.
//
// All of this comes from Substrike, where it was written for its effect slots.
// Nothing is linked across plugin folders in this repository, so it is copied
// and belongs to SaeureKiste from here on -- the same bargain the drive stage
// made. What is different: the namespace, and the drive stage it has to fit
// beside is this plugin's own.
//
// Sources, named where they are used:
//
//   [Simper]  A. Simper, "Linear Trapezoidal Integrated State Variable Filter",
//             Cytomic, 2013. The topology below, which stays stable under the
//             per-sample cutoff modulation a filter envelope needs.
//   [RBJ]     R. Bristow-Johnson, "Cookbook formulae for audio EQ biquad
//             filter coefficients". The shelves, the bell and the peak.
//   [deSoras] L. de Soras, "hiir" -- polyphase IIR halfband filters, designed
//             with its elliptic procedure and checked against the transfer
//             function 1/2 (A0(z^2) + z^-1 A1(z^2)).
//
// None of these books is in this repository and none may be: they are not ours
// to redistribute. What is here is the arithmetic, with the citation beside it.

#include <algorithm>
#include <array>
#include <cmath>

namespace saeurekiste {

constexpr double kChainPi = 3.14159265358979323846;

inline double chainClamp(double v, double lo, double hi) {
   return v < lo ? lo : (v > hi ? hi : v);
}
inline double chainDbToGain(double db) { return std::pow(10.0, db / 20.0); }
// Samples for a time in milliseconds, at least one.
inline double chainSamples(double ms, double rate) {
   return std::max(ms * 0.001 * rate, 1.0);
}

// ----------------------------------------------------------- state variable

// [Simper]. Trapezoidal integration: stable under per-sample cutoff
// modulation, which the filter slot's envelope relies on.
class ChainSvf {
public:
   void reset() { ic1_ = ic2_ = 0.0; }

   void setup(double cutoff, double q, double sampleRate) {
      const double fc = chainClamp(cutoff, 10.0, 0.45 * sampleRate);
      const double g = std::tan(kChainPi * fc / sampleRate);
      k_ = 1.0 / std::max(q, 0.05);
      a1_ = 1.0 / (1.0 + g * (g + k_));
      a2_ = g * a1_;
      a3_ = g * a2_;
   }

   // All three outputs at once: low, band (peak gain Q) and high.
   void tick(double x, double &low, double &band, double &high) {
      const double v3 = x - ic2_;
      const double v1 = a1_ * ic1_ + a2_ * v3;
      const double v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
      ic1_ = 2.0 * v1 - ic1_;
      ic2_ = 2.0 * v2 - ic2_;
      low = v2;
      band = v1;
      high = x - k_ * v1 - v2;
   }

   double lowPass(double x) {
      double l, b, h;
      tick(x, l, b, h);
      return l;
   }

   double highPass(double x) {
      double l, b, h;
      tick(x, l, b, h);
      return h;
   }

   // Unity magnitude, phase only: the low and high outputs minus the band.
   double allPass(double x) {
      double l, b, h;
      tick(x, l, b, h);
      return x - 2.0 * k_ * b;
   }

   // 1/Q, which the shaped outputs (notch, bell) are built with.
   double k() const { return k_; }

private:
   double ic1_ = 0.0, ic2_ = 0.0;
   double k_ = 1.0, a1_ = 1.0, a2_ = 0.0, a3_ = 0.0;
};

// Resonance 0..1 onto Q 0.5..20, logarithmically.
inline double chainResonanceToQ(double r) { return 0.5 * std::pow(40.0, chainClamp(r, 0.0, 1.0)); }

// --------------------------------------------------------------- one-pole

// The interface is float like the drive stage that uses them, but the state is
// double: at an oversampled rate a 5 Hz corner makes the coefficient small
// enough that a float state stops moving short of a constant input, and the
// offset that leaves never decays.
class ChainOnePoleLp {
public:
   void reset() { z_ = 0.0; }
   void setCutoff(float cutoffHz, float sampleRate) {
      const float f = std::max(1.0e-5f, std::min(cutoffHz / sampleRate, 0.49f));
      coef_ = 1.0 - std::exp(-6.283185307179586 * f);
   }
   float tick(float in) {
      z_ += coef_ * (in - z_);
      return static_cast<float>(z_);
   }

private:
   double z_ = 0.0;
   double coef_ = 0.5;
};

class ChainOnePoleHp {
public:
   void reset() { z_ = 0.0; }
   void setCutoff(float cutoffHz, float sampleRate) {
      const float f = std::max(1.0e-5f, std::min(cutoffHz / sampleRate, 0.49f));
      coef_ = 1.0 - std::exp(-6.283185307179586 * f);
   }
   float tick(float in) {
      z_ += coef_ * (in - z_);
      return static_cast<float>(in - z_);
   }

private:
   double z_ = 0.0;
   double coef_ = 0.01;
};

// ----------------------------------------------------------------- biquad

// Transposed direct form II.
struct ChainBiquad {
   double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
   double z1 = 0, z2 = 0;
   void reset() { z1 = z2 = 0.0; }
   double tick(double x) {
      const double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
   }

   // [RBJ]'s shelf and bell formulae.
   void lowShelf(double f, double db, double q, double rate);
   void highShelf(double f, double db, double q, double rate);
   void peak(double f, double db, double q, double rate);
};

// ------------------------------------------------------------- crossovers

// Linkwitz-Riley (24 dB/oct) from Butterworth SVFs: an LR4 lowpass is a
// Butterworth lowpass squared, and its sum with the matching highpass is a
// second-order allpass with Q = 1/sqrt(2). That last fact is what keeps the
// three-way split flat: the low band goes through the upper crossover's
// allpass, so low + mid + high is x through two allpasses -- flat in magnitude
// whatever the two frequencies are. That is what lets a slot process one band
// and pass the rest by it without a level change at the crossings.
class ChainCrossover2 {
public:
   void setup(double freq, double sampleRate) {
      constexpr double q = 0.7071067811865476;
      first_.setup(freq, q, sampleRate);
      low_.setup(freq, q, sampleRate);
      high_.setup(freq, q, sampleRate);
   }

   void reset() {
      first_.reset();
      low_.reset();
      high_.reset();
   }

   void split(double x, double &lo, double &hi) {
      double l, b, h;
      first_.tick(x, l, b, h);
      lo = low_.lowPass(l);
      hi = high_.highPass(h);
   }

private:
   ChainSvf first_, low_, high_;
};

class ChainCrossover3 {
public:
   void setup(double lowFreq, double highFreq, double sampleRate) {
      lower_.setup(lowFreq, sampleRate);
      upper_.setup(highFreq, sampleRate);
      allpass_.setup(highFreq, 0.7071067811865476, sampleRate);
   }

   void reset() {
      lower_.reset();
      upper_.reset();
      allpass_.reset();
   }

   void split(double x, double &lo, double &mid, double &hi) {
      double rest;
      lower_.split(x, lo, rest);
      upper_.split(rest, mid, hi);
      lo = allpass_.allPass(lo);
   }

private:
   ChainCrossover2 lower_, upper_;
   ChainSvf allpass_;
};

// ------------------------------------------------------------ over samplers

// [deSoras]: two chains of first-order allpasses in z^-2, one per phase. No
// latency, only phase, which matters because these sit in a chain whose whole
// point is the transient at its front.
class ChainHalfband {
public:
   static constexpr int kMaxCoefs = 8;

   void init(const double *coefs, int count) {
      count_ = count;
      for (int i = 0; i < count; ++i)
         c_[static_cast<size_t>(i)] = coefs[i];
      reset();
   }

   void reset() {
      x_.fill(0.0);
      y_.fill(0.0);
   }

   // One sample in at the low rate, two out at the high rate.
   void up(double in, double &out0, double &out1) {
      out0 = path(0, in);
      out1 = path(1, in);
   }

   // Two samples in at the high rate, one out at the low rate.
   double down(double in0, double in1) { return 0.5 * (path(0, in1) + path(1, in0)); }

private:
   // Phase p runs the coefficients p, p + 2, p + 4, ...
   double path(int p, double v) {
      for (int i = p; i < count_; i += 2) {
         const size_t k = static_cast<size_t>(i);
         const double t = c_[k] * (v - y_[k]) + x_[k];
         x_[k] = v;
         y_[k] = t;
         v = t;
      }
      return v;
   }

   std::array<double, kMaxCoefs> c_{}, x_{}, y_{};
   int count_ = 0;
};

// 1x, 2x or 4x for one channel. The first stage is the steep one: flat to 0.42
// of the base rate, 99 dB down from 0.58. The second, at twice that rate, only
// has to clear what the first left, and is cheaper for it.
class ChainOversampler {
public:
   static constexpr int kMaxFactor = 4;

   ChainOversampler() {
      static const double kStage1[8] = {0.04063346092419326, 0.1505051290226746, 0.30075705599187408,
                                        0.46077450496145061, 0.6095243148961883, 0.73850384111885725,
                                        0.84922381039206607, 0.9497427837050002};
      static const double kStage2[5] = {0.050857024439668115, 0.18898752479217637, 0.38248720173098638,
                                        0.6052535917518781, 0.85542250996786606};
      up1_.init(kStage1, 8);
      down1_.init(kStage1, 8);
      up2_.init(kStage2, 5);
      down2_.init(kStage2, 5);
   }

   void setFactor(int factor) {
      factor_ = factor >= 4 ? 4 : factor >= 2 ? 2 : 1;
      reset();
   }
   int factor() const { return factor_; }

   void reset() {
      up1_.reset();
      up2_.reset();
      down1_.reset();
      down2_.reset();
   }

   // n samples in, n * factor() out.
   void up(const float *in, float *out, int n) {
      for (int i = 0; i < n; ++i) {
         if (factor_ == 1) {
            out[i] = in[i];
            continue;
         }
         double a, b;
         up1_.up(in[i], a, b);
         if (factor_ == 2) {
            out[2 * i] = static_cast<float>(a);
            out[2 * i + 1] = static_cast<float>(b);
            continue;
         }
         double a0, a1, b0, b1;
         up2_.up(a, a0, a1);
         up2_.up(b, b0, b1);
         out[4 * i] = static_cast<float>(a0);
         out[4 * i + 1] = static_cast<float>(a1);
         out[4 * i + 2] = static_cast<float>(b0);
         out[4 * i + 3] = static_cast<float>(b1);
      }
   }

   // n * factor() samples in, n out.
   void down(const float *in, float *out, int n) {
      for (int i = 0; i < n; ++i) {
         if (factor_ == 1)
            out[i] = in[i];
         else if (factor_ == 2)
            out[i] = static_cast<float>(down1_.down(in[2 * i], in[2 * i + 1]));
         else {
            const double a = down2_.down(in[4 * i], in[4 * i + 1]);
            const double b = down2_.down(in[4 * i + 2], in[4 * i + 3]);
            out[i] = static_cast<float>(down1_.down(a, b));
         }
      }
   }

private:
   ChainHalfband up1_, up2_, down1_, down2_;
   int factor_ = 1;
};

// ------------------------------------------------------------- delay lines

// A delay line over memory the slot owns. read() before push(): read(d) is the
// sample pushed d samples ago, so read(1) is the last one.
struct ChainDelayLine {
   float *buf = nullptr;
   int size = 0;
   int w = 0;

   void attach(float *mem, int n) {
      buf = mem;
      size = n;
      w = 0;
   }
   void reset() { w = 0; }
   void push(float x) {
      buf[w] = x;
      if (++w == size)
         w = 0;
   }
   float at(int d) const {
      int i = w - d;
      if (i < 0)
         i += size;
      return buf[i];
   }
   // A fractional delay of at least 2 samples, four-point Hermite.
   double read(double d) const;
};

// A Schroeder allpass on a delay line. The length is whole samples: an
// interpolated read inside the loop would be a lowpass, and the allpass would
// no longer pass everything.
struct ChainAllpass {
   ChainDelayLine line;
   double tick(double x, int length, double g) {
      const double d = line.at(length);
      const double v = x + g * d;
      line.push(static_cast<float>(v));
      return d - g * v;
   }
};

} // namespace saeurekiste
