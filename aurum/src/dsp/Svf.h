#pragma once

#include <cmath>
#include <complex>

#include "Math.h"

namespace aurum::dsp {

// Coefficients of a linear trapezoidal state-variable filter (A. Simper,
// "Linear Trap Optimised SVF"). Output = m0*v0 + m1*v1 + m2*v2.
struct SvfCoeffs
{
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;
    float m0 = 1.0f, m1 = 0.0f, m2 = 0.0f;

    static SvfCoeffs fromGK(double g, double k, double m0, double m1, double m2)
    {
        SvfCoeffs c;
        const double a1 = 1.0 / (1.0 + g * (g + k));
        c.a1 = static_cast<float>(a1);
        c.a2 = static_cast<float>(g * a1);
        c.a3 = static_cast<float>(g * g * a1);
        c.m0 = static_cast<float>(m0);
        c.m1 = static_cast<float>(m1);
        c.m2 = static_cast<float>(m2);
        return c;
    }

    static double prewarp(double freq, double sampleRate)
    {
        const double f = clamp(freq, 1.0, sampleRate * 0.49);
        return std::tan(kPi * f / sampleRate);
    }

    static SvfCoeffs identity() { return SvfCoeffs{}; }

    static SvfCoeffs bell(double freq, double q, double gainDb, double fs)
    {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double g = prewarp(freq, fs);
        const double k = 1.0 / (q * A);
        return fromGK(g, k, 1.0, k * (A * A - 1.0), 0.0);
    }
    static SvfCoeffs lowShelf(double freq, double q, double gainDb, double fs)
    {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double g = prewarp(freq, fs) / std::sqrt(A);
        const double k = 1.0 / q;
        return fromGK(g, k, 1.0, k * (A - 1.0), A * A - 1.0);
    }
    static SvfCoeffs highShelf(double freq, double q, double gainDb, double fs)
    {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double g = prewarp(freq, fs) * std::sqrt(A);
        const double k = 1.0 / q;
        return fromGK(g, k, A * A, k * (1.0 - A) * A, 1.0 - A * A);
    }
    static SvfCoeffs lowPass(double freq, double q, double fs)
    {
        return fromGK(prewarp(freq, fs), 1.0 / q, 0.0, 0.0, 1.0);
    }
    static SvfCoeffs highPass(double freq, double q, double fs)
    {
        const double k = 1.0 / q;
        return fromGK(prewarp(freq, fs), k, 1.0, -k, -1.0);
    }
    static SvfCoeffs bandPass(double freq, double q, double fs)
    {
        return fromGK(prewarp(freq, fs), 1.0 / q, 0.0, 1.0, 0.0);
    }
    static SvfCoeffs notch(double freq, double q, double fs)
    {
        const double k = 1.0 / q;
        return fromGK(prewarp(freq, fs), k, 1.0, -k, 0.0);
    }
};

struct SvfState
{
    float ic1 = 0.0f, ic2 = 0.0f;
    void reset() { ic1 = ic2 = 0.0f; }

    inline float process(float v0, const SvfCoeffs& c)
    {
        const float v3 = v0 - ic2;
        const float v1 = c.a1 * ic1 + c.a2 * v3;
        const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
    }
};

// One-pole TPT filter (lowpass/highpass), used for odd-order cuts and tone.
struct OnePoleCoeffs
{
    float G = 0.0f;
    static OnePoleCoeffs make(double freq, double fs)
    {
        const double g = SvfCoeffs::prewarp(freq, fs);
        OnePoleCoeffs c;
        c.G = static_cast<float>(g / (1.0 + g));
        return c;
    }
};

struct OnePoleState
{
    float s = 0.0f;
    void reset() { s = 0.0f; }
    inline float lowpass(float x, const OnePoleCoeffs& c)
    {
        const float v = (x - s) * c.G;
        const float y = v + s;
        s = y + v;
        return y;
    }
    inline float highpass(float x, const OnePoleCoeffs& c) { return x - lowpass(x, c); }
};

// Exact magnitude responses of the bilinear filters above, evaluated through
// the warped analog prototype. Used for curve display, GEQ design and gain
// compensation.
namespace response {

inline double warpRatio(double f, double f0, double fs)
{
    const double nyq = fs * 0.4999;
    const double wf = std::tan(kPi * std::min(f, nyq) / fs);
    const double w0 = std::tan(kPi * clamp(f0, 1.0, fs * 0.49) / fs);
    return wf / w0;
}

inline double bellPow(double f, double f0, double q, double gainDb, double fs)
{
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w = warpRatio(f, f0, fs);
    const double re = 1.0 - w * w;
    const double imN = w * A / q;
    const double imD = w / (A * q);
    return (re * re + imN * imN) / (re * re + imD * imD);
}

inline double lowShelfPow(double f, double f0, double q, double gainDb, double fs)
{
    // H(s) = A * (s^2 + sqrt(A)/Q s + A) / (A s^2 + sqrt(A)/Q s + 1)
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w = warpRatio(f, f0, fs);
    const double sa = std::sqrt(A);
    const std::complex<double> s(0.0, w);
    const std::complex<double> num = A * (s * s + sa / q * s + A);
    const std::complex<double> den = A * s * s + sa / q * s + 1.0;
    return std::norm(num / den);
}

inline double highShelfPow(double f, double f0, double q, double gainDb, double fs)
{
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w = warpRatio(f, f0, fs);
    const double sa = std::sqrt(A);
    const std::complex<double> s(0.0, w);
    const std::complex<double> num = A * (A * s * s + sa / q * s + 1.0);
    const std::complex<double> den = s * s + sa / q * s + A;
    return std::norm(num / den);
}

inline double lowPassPow(double f, double f0, double q, double fs)
{
    const double w = warpRatio(f, f0, fs);
    const double re = 1.0 - w * w;
    const double im = w / q;
    return 1.0 / (re * re + im * im);
}

inline double highPassPow(double f, double f0, double q, double fs)
{
    const double w = warpRatio(f, f0, fs);
    const double re = 1.0 - w * w;
    const double im = w / q;
    return (w * w * w * w) / (re * re + im * im);
}

inline double notchPow(double f, double f0, double q, double fs)
{
    const double w = warpRatio(f, f0, fs);
    const double re = 1.0 - w * w;
    const double im = w / q;
    return (re * re) / (re * re + im * im);
}

inline double onePoleLowPow(double f, double f0, double fs)
{
    const double w = warpRatio(f, f0, fs);
    return 1.0 / (1.0 + w * w);
}

inline double onePoleHighPow(double f, double f0, double fs)
{
    const double w = warpRatio(f, f0, fs);
    return (w * w) / (1.0 + w * w);
}

} // namespace response

} // namespace aurum::dsp
