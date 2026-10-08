#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "plugin/HitRender.h"

namespace substrike::gui {

// A hit rendered for drawing, reduced to a peak envelope per bin, with the
// render itself kept for the export.
struct HitView
{
    static constexpr int kBins = 1024;
    std::shared_ptr<const HitRender> render;
    double seconds = 0.0;
    // Peak of |l| and |r| per bin; [0] is the main output, [1 + L] lane L.
    std::array<std::array<float, kBins>, 1 + dsp::kNumLanes> peaks{};
    std::array<float, 1 + dsp::kNumLanes> max{};
};

// Renders hits on a worker thread so a long tail never stalls the window.
// One render at a time; a request while one runs is kept and started when it
// finishes, so the newest state always gets drawn. Main thread only.
class HitPreview
{
public:
    ~HitPreview();

    void request(const std::vector<double>& values, const std::array<dsp::Curve, dsp::kNumCurves>& curves,
                 double sampleRate);
    // Picks up a finished render; true when the view changed.
    bool poll();
    const HitView* view() const { return view_.get(); }

    // The longest hit the preview and the export render.
    static constexpr double kMaxSeconds = 8.0;

private:
    struct Job
    {
        std::vector<double> values;
        std::array<dsp::Curve, dsp::kNumCurves> curves;
        double sampleRate;
    };
    void start(Job job);

    std::thread thread_;
    std::atomic<bool> done_{false};
    bool running_ = false;
    std::unique_ptr<Job> queued_;
    std::shared_ptr<HitView> result_; // written by the worker until done_
    std::shared_ptr<const HitView> view_;
};

} // namespace substrike::gui
