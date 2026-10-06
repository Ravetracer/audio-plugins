#pragma once

#include "Math.h"

namespace aurum::dsp {

// Smoothly interpolated random signal in [-1, 1] (cubic smoothstep between
// random targets). Advanced in blocks; cheap enough for one per delay line.
struct SmoothRandom
{
    Rng rng;
    float prev = 0.0f;
    float next = 0.0f;
    float phase = 0.0f;
    float rate = 0.5f; // targets per second
    float rateJitter = 1.0f;

    void init(uint32_t seed, float jitter)
    {
        rng = Rng(seed);
        prev = rng.bipolar();
        next = rng.bipolar();
        phase = rng.uniform();
        rateJitter = jitter;
    }

    float advance(int samples, double sampleRate)
    {
        phase += rate * rateJitter * static_cast<float>(samples / sampleRate);
        while (phase >= 1.0f)
        {
            phase -= 1.0f;
            prev = next;
            next = rng.bipolar();
        }
        const float t = phase;
        const float sm = t * t * (3.0f - 2.0f * t);
        return prev + (next - prev) * sm;
    }
};

} // namespace aurum::dsp
