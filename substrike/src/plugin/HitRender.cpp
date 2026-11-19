#include "HitRender.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>

#include "Params.h"
#include "Player.h"
#include "util/Path.h"

namespace substrike {

HitRenderer::HitRenderer(const std::vector<double>& values, const std::array<dsp::Curve, dsp::kNumCurves>& curves,
                         double sampleRate, double maxSeconds)
{
    out_.sampleRate = sampleRate;
    // The engine is large (voices, chains, oversamplers): keep it off the stack.
    engine_ = std::make_unique<Player>();
    engine_->setTapLanes(true);
    engine_->setValues(values.data());
    engine_->prepare(sampleRate);
    for (int i = 0; i < dsp::kNumCurves; ++i)
        engine_->curve(i) = curves[static_cast<size_t>(i)];
    engine_->noteOn(engine_->rootNote(), 1.0);
    limit_ = static_cast<size_t>(std::max(1.0, maxSeconds * sampleRate));
}

HitRenderer::~HitRenderer() = default;

bool HitRenderer::step(int blocks)
{
    dsp::Bus buses[dsp::Engine::kNumBuses];
    for (int b = 0; b < dsp::Engine::kNumBuses; ++b)
        buses[b] = {buf_[static_cast<size_t>(2 * b)].data(), buf_[static_cast<size_t>(2 * b + 1)].data()};

    for (int k = 0; k < blocks && !finished_; ++k)
    {
        if (done_ >= limit_)
        {
            finished_ = true;
            break;
        }
        const int n = static_cast<int>(std::min<size_t>(kBlock, limit_ - done_));
        engine_->process(buses, n);
        out_.left.insert(out_.left.end(), buses[0].l, buses[0].l + n);
        out_.right.insert(out_.right.end(), buses[0].r, buses[0].r + n);
        for (int l = 0; l < dsp::kNumLanes; ++l)
        {
            out_.laneLeft[static_cast<size_t>(l)].insert(out_.laneLeft[static_cast<size_t>(l)].end(), buses[1 + l].l,
                                                         buses[1 + l].l + n);
            out_.laneRight[static_cast<size_t>(l)].insert(out_.laneRight[static_cast<size_t>(l)].end(),
                                                          buses[1 + l].r, buses[1 + l].r + n);
        }
        done_ += static_cast<size_t>(n);
        if (engine_->idle())
        {
            out_.complete = true;
            finished_ = true;
        }
        else if (done_ >= limit_)
            finished_ = true;
    }
    if (finished_ && engine_)
    {
        for (size_t i = 0; i < out_.left.size(); ++i)
            out_.peak = std::max({out_.peak, std::fabs(out_.left[i]), std::fabs(out_.right[i])});
        engine_.reset();
    }
    return finished_;
}

HitRender HitRenderer::take() { return std::move(out_); }

HitRender renderHit(const std::vector<double>& values, const std::array<dsp::Curve, dsp::kNumCurves>& curves,
                    double sampleRate, double maxSeconds)
{
    HitRenderer r(values, curves, sampleRate, maxSeconds);
    while (!r.step(1 << 30))
    {
    }
    return r.take();
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
