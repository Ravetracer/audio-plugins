#pragma once

#include <array>
#include <cmath>
#include <vector>

#include "Math.h"
#include "Svf.h"

namespace aurum::dsp {

// Third-octave graphic EQ designer used to realise frequency dependent decay
// inside recirculating delay structures.
//
// Based on the accurate cascade graphic EQ (Välimäki & Liski, 2017) and its use
// for FDN reverberation time control (Prawda, Schlecht, Välimäki, 2019). The dB
// response of each band is treated as linear in its command gain around a
// prototype gain. Two changes make it suit decay control:
//  - the least-squares fit is weighted by 1/|target| so it minimises relative
//    (i.e. T60) error rather than absolute dB error, and
//  - shelving bands at both ends keep control outside 25 Hz .. 20 kHz.
// All delay lines share one target *shape* (attenuation per sample); a line of
// length d needs `d * shape`. The weighted pseudo-inverse therefore only has to
// be rebuilt when the shape changes, and each line only pays for a refinement
// pass that corrects the nonlinearity at its actual gains.
class GeqDesigner
{
public:
    enum class Kind : uint8_t { Bell, LowShelf, HighShelf };

    static constexpr int kMaxBands = 34;
    static constexpr int kMaxPoints = 96;
    static constexpr double kMinTargetDb = -30.0; // deeper attenuation per pass is irrelevant
    // Band gains are kept in a moderate range: large opposing gains cancel in
    // the steady state but make coefficient updates produce large transients.
    static constexpr double kMinBandDb = -36.0;
    static constexpr double kMaxBandDb = 18.0;

    void prepare(double sampleRate);

    int numBands() const { return numBands_; }
    int numPoints() const { return numPoints_; }
    Kind bandKind(int k) const { return kinds_[static_cast<size_t>(k)]; }
    double bandFreq(int k) const { return freqs_[static_cast<size_t>(k)]; }
    double bandQ(int k) const { return qs_[static_cast<size_t>(k)]; }
    double pointFreq(int m) const { return points_[static_cast<size_t>(m)]; }
    double sampleRate() const { return fs_; }

    // Set the attenuation shape in dB per sample at every design point (all <= 0).
    void setShape(const double* shapeDbPerSample);

    // Design gains for a line of `delaySamples` using the current shape.
    void design(double delaySamples, double* bandGainsDb, double& broadbandDb, int refinePasses = 8) const;

    // Design all lines of a structure at once: a few reference delays spanning
    // the range are designed exactly, the per-line gains are interpolated from
    // them (gain per sample varies smoothly with the delay). `gains` is an
    // n x kMaxBands row-major array.
    void designLines(const double* delays, int n, double* gains, double* broadband) const;

    // Generic fit to an arbitrary target (used by tests and the IR analyser).
    void designTarget(const double* targetDb, double* bandGainsDb, double& broadbandDb, int refinePasses) const;

    double realisedDbAt(const double* bandGainsDb, double broadbandDb, double freq) const;

    double qFactor = 0.35; // bandwidth multiplier relative to nominal third-octave Q

private:
    double bandPowAt(int k, double gainDb, double freq) const;
    void buildInteraction();
    void buildWeightedPinv(const double* weights, std::vector<double>& pinv) const;
    void applyPinv(const std::vector<double>& pinv, const double* t, double* gains, double& bb) const;
    void realisedAtPoints(const double* gains, double bb, double* out) const;
    void refine(const std::vector<double>& pinv, const double* targetDb, double* gains, double& bb,
                int maxPasses) const;

    double fs_ = 48000.0;
    int numBands_ = 0;
    int numPoints_ = 0;
    std::array<Kind, kMaxBands> kinds_{};
    std::array<double, kMaxBands> freqs_{};
    std::array<double, kMaxBands> qs_{};
    std::array<double, kMaxPoints> points_{};
    // Warped frequency ratio w = tan(pi f/fs)/tan(pi f0/fs) for each (band, point).
    std::vector<double> warp_;
    // Interaction matrix B[m][k] at the prototype gain (dB per dB).
    std::vector<double> B_;
    // Current shape and its weighted pseudo-inverse.
    std::array<double, kMaxPoints> shape_{};
    std::vector<double> pinvShape_;
    std::array<double, kMaxBands + 1> shapeSolution_{};
    // Scratch buffers (sized in prepare, so design() never allocates).
    mutable std::vector<double> scratchPinv_;
    mutable std::vector<double> normal_;
    mutable std::vector<double> normalInv_;
};

// A bank of cascaded SVF sections for L parallel lanes (one lane per delay
// line). Coefficients are stored band-major so that the loop over lanes
// vectorises.
template <int L> class AttenuationBank
{
public:
    AttenuationBank() { setIdentity(); }

    void reset()
    {
        for (auto& row : ic1_)
            row.fill(0.0f);
        for (auto& row : ic2_)
            row.fill(0.0f);
    }

    // Lossless pass-through (used for freeze and before the first design).
    void setIdentity()
    {
        numBands_ = 0;
        broadband_.fill(1.0f);
    }

    // Load lanes [offset, offset + L) from a coefficient set (see DecayCoeffSet).
    template <typename Set> void loadCoeffs(const Set& set, int offset)
    {
        numBands_ = set.numBands;
        for (int k = 0; k < numBands_; ++k)
            for (int i = 0; i < L; ++i)
            {
                const int src = offset + i;
                if (src < set.numLanes)
                {
                    a1_[k][i] = set.a1[k][src];
                    a2_[k][i] = set.a2[k][src];
                    a3_[k][i] = set.a3[k][src];
                    m0_[k][i] = set.m0[k][src];
                    m1_[k][i] = set.m1[k][src];
                    m2_[k][i] = set.m2[k][src];
                }
            }
        for (int i = 0; i < L; ++i)
            if (offset + i < set.numLanes)
                broadband_[i] = set.broadband[offset + i];
    }

    inline void process(float* x)
    {
        alignas(64) float v[L];
        for (int i = 0; i < L; ++i)
            v[i] = x[i] * broadband_[i];
        for (int k = 0; k < numBands_; ++k)
        {
            float* __restrict c1 = ic1_[k].data();
            float* __restrict c2 = ic2_[k].data();
            const float* __restrict a1 = a1_[k].data();
            const float* __restrict a2 = a2_[k].data();
            const float* __restrict a3 = a3_[k].data();
            const float* __restrict m0 = m0_[k].data();
            const float* __restrict m1 = m1_[k].data();
            const float* __restrict m2 = m2_[k].data();
            for (int i = 0; i < L; ++i)
            {
                const float v0 = v[i];
                const float v3 = v0 - c2[i];
                const float v1 = a1[i] * c1[i] + a2[i] * v3;
                const float v2 = c2[i] + a2[i] * c1[i] + a3[i] * v3;
                c1[i] = 2.0f * v1 - c1[i];
                c2[i] = 2.0f * v2 - c2[i];
                v[i] = m0[i] * v0 + m1[i] * v1 + m2[i] * v2;
            }
        }
        for (int i = 0; i < L; ++i)
            x[i] = v[i];
    }

private:
    using Row = std::array<float, L>;
    int numBands_ = 0;
    alignas(64) std::array<Row, GeqDesigner::kMaxBands> a1_{};
    alignas(64) std::array<Row, GeqDesigner::kMaxBands> a2_{};
    alignas(64) std::array<Row, GeqDesigner::kMaxBands> a3_{};
    alignas(64) std::array<Row, GeqDesigner::kMaxBands> m0_{};
    alignas(64) std::array<Row, GeqDesigner::kMaxBands> m1_{};
    alignas(64) std::array<Row, GeqDesigner::kMaxBands> m2_{};
    alignas(64) std::array<Row, GeqDesigner::kMaxBands> ic1_{};
    alignas(64) std::array<Row, GeqDesigner::kMaxBands> ic2_{};
    alignas(64) Row broadband_{};
};

} // namespace aurum::dsp
