#include "HitPreview.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace substrike::gui {

namespace {

void reduce(const std::vector<float>& l, const std::vector<float>& r, std::array<float, HitView::kBins>& peaks,
            float& max, size_t total)
{
    peaks.fill(0.0f);
    max = 0.0f;
    if (total == 0)
        return;
    for (size_t i = 0; i < l.size(); ++i)
    {
        const size_t bin = std::min<size_t>(HitView::kBins - 1, i * HitView::kBins / total);
        const float v = std::max(std::fabs(l[i]), std::fabs(r[i]));
        peaks[bin] = std::max(peaks[bin], v);
    }
    for (float v : peaks)
        max = std::max(max, v);
}

// The peak envelopes of a finished render, and the render itself.
void finish(HitView& out, std::shared_ptr<HitRender> render, double sampleRate)
{
    const size_t total = render->left.size();
    out.seconds = static_cast<double>(total) / sampleRate;
    reduce(render->left, render->right, out.peaks[0], out.max[0], total);
    for (int l = 0; l < dsp::kNumLanes; ++l)
        reduce(render->laneLeft[static_cast<size_t>(l)], render->laneRight[static_cast<size_t>(l)],
               out.peaks[static_cast<size_t>(1 + l)], out.max[static_cast<size_t>(1 + l)], total);
    out.render = std::move(render);
}

} // namespace

#if SUBSTRIKE_PREVIEW_THREAD

HitPreview::~HitPreview()
{
    if (thread_.joinable())
        thread_.join();
}

void HitPreview::request(const std::vector<double>& values, const std::array<dsp::Curve, dsp::kNumCurves>& curves,
                         double sampleRate)
{
    Job job{values, curves, sampleRate};
    if (running_)
    {
        queued_ = std::make_unique<Job>(std::move(job));
        return;
    }
    start(std::move(job));
}

void HitPreview::start(Job job)
{
    if (thread_.joinable())
        thread_.join();
    running_ = true;
    done_.store(false);
    result_ = std::make_shared<HitView>();
    thread_ = std::thread([this, job = std::move(job), out = result_]() {
        finish(*out, std::make_shared<HitRender>(renderHit(job.values, job.curves, job.sampleRate, kMaxSeconds)),
               job.sampleRate);
        done_.store(true, std::memory_order_release);
    });
}

bool HitPreview::poll()
{
    if (!running_ || !done_.load(std::memory_order_acquire))
        return false;
    thread_.join();
    running_ = false;
    view_ = std::move(result_);
    if (queued_)
    {
        std::unique_ptr<Job> next = std::move(queued_);
        start(std::move(*next));
    }
    return true;
}

#else

HitPreview::~HitPreview() = default;

void HitPreview::request(const std::vector<double>& values, const std::array<dsp::Curve, dsp::kNumCurves>& curves,
                         double sampleRate)
{
    Job job{values, curves, sampleRate};
    if (running_)
    {
        queued_ = std::make_unique<Job>(std::move(job));
        return;
    }
    start(std::move(job));
}

void HitPreview::start(Job job)
{
    running_ = true;
    sampleRate_ = job.sampleRate;
    renderer_ = std::make_unique<HitRenderer>(job.values, job.curves, job.sampleRate, kMaxSeconds);
}

bool HitPreview::poll()
{
    if (!running_)
        return false;
    // About a third of a frame at 60 Hz, checked every few blocks; more while
    // there is nothing to show at all, which is most often all of a hit.
    using clock = std::chrono::steady_clock;
    const auto until = clock::now() + std::chrono::milliseconds(view_ ? 6 : 100);
    while (!renderer_->step(4))
        if (clock::now() >= until)
            return false;
    auto out = std::make_shared<HitView>();
    finish(*out, std::make_shared<HitRender>(renderer_->take()), sampleRate_);
    renderer_.reset();
    running_ = false;
    view_ = std::move(out);
    if (queued_)
    {
        std::unique_ptr<Job> next = std::move(queued_);
        start(std::move(*next));
    }
    return true;
}

#endif

} // namespace substrike::gui
