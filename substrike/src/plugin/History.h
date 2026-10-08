#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include "dsp/Engine.h"

namespace substrike {

// Undo and redo over whole snapshots of the state: every parameter's stored
// value and every curve. A snapshot is taken after each finished edit; one
// that equals the current one is not kept. Main thread only.
class History
{
public:
    struct Snapshot
    {
        std::vector<double> values;
        std::array<dsp::Curve, dsp::kNumCurves> curves{};
        std::string preset; // the name the browser shows; not a change by itself
        bool operator==(const Snapshot& o) const { return values == o.values && curves == o.curves; }
    };

    static constexpr size_t kDepth = 100;

    // Forgets everything; `s` is where undo stops.
    void reset(Snapshot s)
    {
        steps_.clear();
        steps_.push_back(std::move(s));
        at_ = 0;
    }
    // Keeps `s` as the newest step, dropping what could have been redone.
    // False when nothing changed.
    bool record(Snapshot s)
    {
        if (!steps_.empty() && steps_[at_] == s)
            return false;
        steps_.erase(steps_.begin() + static_cast<std::ptrdiff_t>(steps_.empty() ? 0 : at_ + 1), steps_.end());
        steps_.push_back(std::move(s));
        if (steps_.size() > kDepth)
            steps_.pop_front();
        at_ = steps_.size() - 1;
        return true;
    }
    bool canUndo() const { return at_ > 0; }
    bool canRedo() const { return at_ + 1 < steps_.size(); }
    const Snapshot* undo() { return canUndo() ? &steps_[--at_] : nullptr; }
    const Snapshot* redo() { return canRedo() ? &steps_[++at_] : nullptr; }

private:
    std::deque<Snapshot> steps_;
    size_t at_ = 0;
};

} // namespace substrike
