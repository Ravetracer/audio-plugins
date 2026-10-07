#pragma once

#include "dsp/Svf.h"

namespace substrike::dsp {

// Linkwitz-Riley crossovers (24 dB/oct), built from Butterworth SVFs: an LR4
// lowpass is a Butterworth lowpass squared, and its sum with the matching
// highpass is a second-order allpass with Q = 1/sqrt(2). That last fact is what
// keeps the three-way split flat: the low band goes through the upper
// crossover's allpass, so low + mid + high is x through two allpasses -- flat
// in magnitude, whatever the two frequencies are.
class Crossover2
{
public:
    void setup(double freq, double sampleRate)
    {
        constexpr double q = 0.7071067811865476;
        first_.setup(freq, q, sampleRate);
        low_.setup(freq, q, sampleRate);
        high_.setup(freq, q, sampleRate);
    }

    void reset()
    {
        first_.reset();
        low_.reset();
        high_.reset();
    }

    void split(double x, double& lo, double& hi)
    {
        double l, b, h;
        first_.tick(x, l, b, h);
        lo = low_.lowPass(l);
        hi = high_.highPass(h);
    }

private:
    Svf first_, low_, high_;
};

class Crossover3
{
public:
    void setup(double lowFreq, double highFreq, double sampleRate)
    {
        lower_.setup(lowFreq, sampleRate);
        upper_.setup(highFreq, sampleRate);
        allpass_.setup(highFreq, 0.7071067811865476, sampleRate);
    }

    void reset()
    {
        lower_.reset();
        upper_.reset();
        allpass_.reset();
    }

    void split(double x, double& lo, double& mid, double& hi)
    {
        double rest;
        lower_.split(x, lo, rest);
        upper_.split(rest, mid, hi);
        lo = allpass_.allPass(lo);
    }

private:
    Crossover2 lower_, upper_;
    Svf allpass_;
};

} // namespace substrike::dsp
