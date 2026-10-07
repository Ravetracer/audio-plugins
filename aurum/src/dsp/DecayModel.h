#pragma once

#include <array>
#include <cmath>

#include "Math.h"

namespace aurum::dsp {

// Shapes available for Decay Rate EQ bands.
enum class DecayShape : int { Bell = 0, LowShelf = 1, HighShelf = 2, Notch = 3 };

struct DecayBand
{
    bool used = false;
    bool enabled = true;
    DecayShape shape = DecayShape::Bell;
    double freq = 1000.0;
    double rateLog2 = 0.0; // decay multiplier as log2 (e.g. -1 = 50 %)
    double q = 1.0;
};

constexpr int kNumDecayBands = 6;

// Frequency-dependent reverberation time. Shared by the DSP (to design the
// attenuation filters) and the GUI (to draw the Decay Rate curve).
struct DecayModel
{
    double baseT60 = 2.0;   // seconds, from Space
    double decayRate = 1.0; // multiplier from the Decay Rate knob
    // Inherent room character (T60 multipliers at the extremes).
    double lfMult = 1.2, lfFreq = 250.0;
    double hfMult = 0.6, hfFreq = 4000.0;
    double brightness = 0.5; // 0..1, 0.5 neutral
    // How much of Brightness reaches the decay, per style (ReverbEngine::
    // decayModelFor): a scale on the absorption of darker settings and the
    // exponent of its rise with frequency, and a scale on the lengthening of
    // brighter ones.
    double brightAbsScale = 1.0, brightAbsExp = 1.05;
    double brightLog2Scale = 1.0;
    // A decay time the style does not go below however short Decay Rate
    // makes it, joined softly (0 = none); see ReverbEngine::decayModelFor.
    double floorT60 = 0.0;
    std::array<DecayBand, kNumDecayBands> bands{};
    // Calibration of the style's decay against reference renders, at the
    // octaves 63 Hz .. 16 kHz (see ReverbEngine::decayModelFor): a log2
    // multiplier on the decay time, and an absorption in 1/s that limits it
    // however long Space and Length make it.
    std::array<double, 9> calib{};
    std::array<double, 9> absorb{};
    // Internal correction of a reverb structure whose measured decay deviates
    // from its nominal loop lengths (applied to the design, not the display).
    double corrScale = 1.0;   // broadband T60 factor
    double corrLfScale = 1.0; // extra factor below corrLfFreq
    double corrLfFreq = 200.0;

    static constexpr double kMinT60 = 0.03;
    static constexpr double kMaxT60 = 120.0;

    // Analog prototype magnitudes (squared), frequency ratio w = f / f0.
    static double bellPow(double w, double q, double gainDb)
    {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double re = 1.0 - w * w;
        const double n = w * A / q, d = w / (A * q);
        return (re * re + n * n) / (re * re + d * d);
    }
    static double lowShelfPow(double w, double q, double gainDb)
    {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w2 = w * w, im2 = w2 * A / (q * q);
        const double a = A - w2, b = 1.0 - A * w2;
        return A * A * (a * a + im2) / (b * b + im2);
    }
    static double highShelfPow(double w, double q, double gainDb)
    {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w2 = w * w, im2 = w2 * A / (q * q);
        const double a = 1.0 - A * w2, b = A - w2;
        return A * A * (a * a + im2) / (b * b + im2);
    }
    static double notchPow(double w, double q)
    {
        const double re = 1.0 - w * w, im = w / q;
        return (re * re) / (re * re + im * im);
    }

    // log2 decay multiplier contributed by one user band at frequency f.
    static double bandLog2(const DecayBand& b, double f)
    {
        if (!b.used || !b.enabled)
            return 0.0;
        constexpr double kDbPerOctave = 6.020599913279624; // 20*log10(2)
        const double w = f / b.freq;
        switch (b.shape)
        {
        case DecayShape::Bell:
            return 10.0 * std::log10(bellPow(w, b.q, b.rateLog2 * kDbPerOctave)) / kDbPerOctave;
        case DecayShape::LowShelf:
            return 10.0 * std::log10(lowShelfPow(w, b.q * kShelfQScale, b.rateLog2 * kDbPerOctave)) / kDbPerOctave;
        case DecayShape::HighShelf:
            return 10.0 * std::log10(highShelfPow(w, b.q * kShelfQScale, b.rateLog2 * kDbPerOctave)) / kDbPerOctave;
        case DecayShape::Notch:
        {
            const double depth = std::pow(2.0, b.rateLog2);
            const double m = 1.0 - (1.0 - depth) * (1.0 - notchPow(w, b.q));
            return std::log2(std::max(m, 1e-3));
        }
        }
        return 0.0;
    }

    // Combined user curve (log2 multiplier).
    double userLog2(double f) const
    {
        double s = 0.0;
        for (const auto& b : bands)
            s += bandLog2(b, f);
        return s;
    }

    // Room + brightness curve (log2 multiplier), without the user bands.
    double roomLog2(double f) const
    {
        const double lw = (lfFreq / f) * (lfFreq / f);
        const double hw = (f / hfFreq) * (f / hfFreq);
        const double s = std::log2(lfMult) * lw / (1.0 + lw) + std::log2(hfMult) * hw / (1.0 + hw);
        return s + calibLog2(f);
    }

    // What Brightness does to the decay; most of what it does is a tone
    // change (EngineControl). Fitted to reference renders of a 2.5 s room at
    // -100 .. +100 %, and to presets with long decays, which showed that a
    // darker setting is an absorption in 1/s (it limits a long tail far more
    // than a short one) while a brighter one only lengthens the highs a
    // little in any room.
    double brightnessAbsorption(double f) const
    {
        const double r = clamp(brightness, 0.0, 1.0) * 2.0 - 1.0;
        if (r >= 0.0 || brightAbsScale <= 0.0)
            return 0.0;
        return brightAbsScale * 0.33 * std::pow(-r, 1.4) * std::pow(std::min(f / 16000.0, 1.3), brightAbsExp);
    }
    double brightnessLog2(double f) const
    {
        const double r = clamp(brightness, 0.0, 1.0) * 2.0 - 1.0;
        if (r <= 0.0)
            return 0.0;
        return brightLog2Scale * 0.2 * std::tanh(3.0 * r) / std::tanh(3.0) * std::pow(std::min(f / 16000.0, 1.3), 0.6);
    }
    double absorbAt(double f) const
    {
        const double pos = std::log2(std::max(f, 1.0) / 62.5);
        if (pos <= 0.0)
            return absorb[0];
        if (pos >= 8.0)
            return absorb[8];
        const int i = static_cast<int>(pos);
        return absorb[static_cast<size_t>(i)] + (absorb[static_cast<size_t>(i + 1)] - absorb[static_cast<size_t>(i)]) * (pos - i);
    }

    // The calibration curve, linear in log frequency between the octaves.
    double calibLog2(double f) const
    {
        const double pos = std::log2(std::max(f, 1.0) / 62.5);
        if (pos <= 0.0)
            return calib[0];
        if (pos >= 8.0)
            return calib[8];
        const int i = static_cast<int>(pos);
        return calib[static_cast<size_t>(i)] + (calib[static_cast<size_t>(i + 1)] - calib[static_cast<size_t>(i)]) * (pos - i);
    }

    double nominalT60() const { return baseT60 * decayRate; }

    double t60At(double f) const
    {
        double t = nominalT60() * std::exp2(roomLog2(f) + userLog2(f) + brightnessLog2(f));
        t = 1.0 / (1.0 / t + absorbAt(f) + brightnessAbsorption(f));
        if (floorT60 > 0.0)
        {
            const double t2 = t * t, f2 = floorT60 * floorT60;
            t = std::sqrt(std::sqrt(t2 * t2 + f2 * f2));
        }
        if (corrScale != 1.0 || corrLfScale != 1.0)
        {
            const double lw = (corrLfFreq / f) * (corrLfFreq / f);
            t *= corrScale * std::pow(corrLfScale, lw / (1.0 + lw));
        }
        return clamp(t, kMinT60, kMaxT60);
    }
};

} // namespace aurum::dsp
