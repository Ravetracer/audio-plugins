#include "HitRender.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>

#include "Params.h"
#include "util/Path.h"

namespace substrike {

HitRender renderHit(const std::vector<double>& values, const std::array<dsp::Curve, dsp::kNumCurves>& curves,
                    double sampleRate, double maxSeconds)
{
    HitRender out;
    out.sampleRate = sampleRate;
    // The engine is large (voices, chains, oversamplers): keep it off the stack.
    auto engine = std::make_unique<dsp::Engine>();
    dsp::EngineParams p = buildEngineParams(values.data());
    p.tapLanes = true;
    engine->prepare(sampleRate);
    for (int i = 0; i < dsp::kNumCurves; ++i)
        engine->curve(i) = curves[static_cast<size_t>(i)];
    engine->noteOn(p.rootNote, 1.0, p);

    constexpr int kBlock = 256;
    const size_t limit = static_cast<size_t>(std::max(1.0, maxSeconds * sampleRate));
    std::array<std::array<float, kBlock>, 2 * dsp::Engine::kNumBuses> buf{};
    dsp::Bus buses[dsp::Engine::kNumBuses];
    for (int b = 0; b < dsp::Engine::kNumBuses; ++b)
        buses[b] = {buf[static_cast<size_t>(2 * b)].data(), buf[static_cast<size_t>(2 * b + 1)].data()};

    size_t done = 0;
    while (done < limit)
    {
        const int n = static_cast<int>(std::min<size_t>(kBlock, limit - done));
        engine->process(buses, n, p);
        out.left.insert(out.left.end(), buses[0].l, buses[0].l + n);
        out.right.insert(out.right.end(), buses[0].r, buses[0].r + n);
        for (int l = 0; l < dsp::kNumLanes; ++l)
        {
            out.laneLeft[static_cast<size_t>(l)].insert(out.laneLeft[static_cast<size_t>(l)].end(), buses[1 + l].l,
                                                        buses[1 + l].l + n);
            out.laneRight[static_cast<size_t>(l)].insert(out.laneRight[static_cast<size_t>(l)].end(),
                                                         buses[1 + l].r, buses[1 + l].r + n);
        }
        done += static_cast<size_t>(n);
        if (engine->idle())
        {
            out.complete = true;
            break;
        }
    }
    for (size_t i = 0; i < out.left.size(); ++i)
        out.peak = std::max({out.peak, std::fabs(out.left[i]), std::fabs(out.right[i])});
    return out;
}

namespace {
void put16(std::ofstream& f, uint16_t v)
{
    const char b[2] = {static_cast<char>(v & 0xff), static_cast<char>(v >> 8)};
    f.write(b, 2);
}
void put32(std::ofstream& f, uint32_t v)
{
    const char b[4] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff),
                       static_cast<char>((v >> 16) & 0xff), static_cast<char>(v >> 24)};
    f.write(b, 4);
}
} // namespace

bool writeHitWav(const std::string& path, const HitRender& hit, bool normalise)
{
    std::ofstream f(toPath(path), std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    const uint32_t frames = static_cast<uint32_t>(hit.left.size());
    const uint32_t rate = static_cast<uint32_t>(std::lround(hit.sampleRate));
    const uint32_t dataBytes = frames * 2 * 3;
    f.write("RIFF", 4);
    put32(f, 36 + dataBytes);
    f.write("WAVEfmt ", 8);
    put32(f, 16);
    put16(f, 1); // PCM
    put16(f, 2);
    put32(f, rate);
    put32(f, rate * 2 * 3);
    put16(f, 2 * 3);
    put16(f, 24);
    f.write("data", 4);
    put32(f, dataBytes);

    // -0.3 dBFS leaves room for the inter-sample peaks a sampler or a DAW's
    // resampling may find.
    const double gain = normalise && hit.peak > 1e-9f ? std::pow(10.0, -0.3 / 20.0) / hit.peak : 1.0;
    std::vector<char> bytes;
    bytes.reserve(dataBytes);
    auto sample = [&](float x) {
        const double v = std::clamp(static_cast<double>(x) * gain, -1.0, 1.0);
        const int32_t q = static_cast<int32_t>(std::lround(v * 8388607.0));
        bytes.push_back(static_cast<char>(q & 0xff));
        bytes.push_back(static_cast<char>((q >> 8) & 0xff));
        bytes.push_back(static_cast<char>((q >> 16) & 0xff));
    };
    for (uint32_t i = 0; i < frames; ++i)
    {
        sample(hit.left[i]);
        sample(hit.right[i]);
    }
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

} // namespace substrike
