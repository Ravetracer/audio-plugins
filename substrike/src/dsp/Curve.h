#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace substrike::dsp {

// The shape of one curve segment: maps 0..1 onto 0..1. A positive curvature
// moves fast at the start of the segment and slowly at its end -- an
// exponential drop or rise -- a negative one the other way round, and zero is a
// straight line. At +1 the segment has covered 95 % of its way after a quarter
// of its length.
inline double segmentShape(double u, double curvature)
{
    const double k = 12.0 * std::clamp(curvature, -1.0, 1.0);
    if (std::fabs(k) < 1e-4)
        return u;
    return (1.0 - std::exp(-k * u)) / (1.0 - std::exp(-k));
}

struct CurvePoint
{
    double x = 0.0; // position, 0..1, ascending
    double y = 0.0; // value, 0..1
    double k = 0.0; // curvature of the segment from here to the next point
};

// A breakpoint curve over a normalised time axis. The engine scales its time
// and its value range with parameters (the macros), so a drawn curve and an
// automated knob work together. `bend` is a macro too: it is added to every
// segment's own curvature.
class Curve
{
public:
    static constexpr int kMaxPoints = 16;

    // A single falling segment, 1 -> 0.
    Curve()
    {
        points_[0] = {0.0, 1.0, 0.0};
        points_[1] = {1.0, 0.0, 0.0};
        count_ = 2;
    }

    // Points must start at x = 0, end at x = 1 and ascend; anything else is
    // repaired rather than rejected.
    void set(const CurvePoint* points, int count)
    {
        count_ = std::clamp(count, 2, kMaxPoints);
        for (int i = 0; i < count_; ++i)
        {
            CurvePoint p = points[i];
            p.x = std::clamp(p.x, i > 0 ? points_[i - 1].x : 0.0, 1.0);
            p.y = std::clamp(p.y, 0.0, 1.0);
            p.k = std::clamp(p.k, -1.0, 1.0);
            points_[i] = p;
        }
        points_[0].x = 0.0;
        points_[count_ - 1].x = 1.0;
    }

    int count() const { return count_; }
    const CurvePoint& point(int i) const { return points_[i]; }

    bool operator==(const Curve& o) const
    {
        if (count_ != o.count_)
            return false;
        for (int i = 0; i < count_; ++i)
            if (points_[i].x != o.points_[i].x || points_[i].y != o.points_[i].y || points_[i].k != o.points_[i].k)
                return false;
        return true;
    }
    bool operator!=(const Curve& o) const { return !(*this == o); }

    double eval(double x, double bend) const
    {
        if (x <= 0.0)
            return points_[0].y;
        if (x >= 1.0)
            return points_[count_ - 1].y;
        int i = 0;
        while (i < count_ - 2 && x >= points_[i + 1].x)
            ++i;
        const CurvePoint& a = points_[i];
        const CurvePoint& b = points_[i + 1];
        const double span = b.x - a.x;
        if (span <= 0.0)
            return b.y;
        const double u = (x - a.x) / span;
        return a.y + (b.y - a.y) * segmentShape(u, a.k + bend);
    }

private:
    std::array<CurvePoint, kMaxPoints> points_{};
    int count_ = 2;
};

} // namespace substrike::dsp
