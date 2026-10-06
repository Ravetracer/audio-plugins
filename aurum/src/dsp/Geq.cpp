#include "Geq.h"

namespace aurum::dsp {

namespace {

// Gauss-Jordan inversion of a dense n x n matrix (row-major, in place).
// `inv` must hold n*n elements; `a` is destroyed and receives the inverse.
bool invert(std::vector<double>& a, std::vector<double>& inv, int n)
{
    std::fill(inv.begin(), inv.begin() + n * n, 0.0);
    for (int i = 0; i < n; ++i)
        inv[static_cast<size_t>(i * n + i)] = 1.0;
    for (int col = 0; col < n; ++col)
    {
        int pivot = col;
        double best = std::fabs(a[static_cast<size_t>(col * n + col)]);
        for (int r = col + 1; r < n; ++r)
        {
            const double v = std::fabs(a[static_cast<size_t>(r * n + col)]);
            if (v > best)
            {
                best = v;
                pivot = r;
            }
        }
        if (best < 1e-300)
            return false;
        if (pivot != col)
            for (int c = 0; c < n; ++c)
            {
                std::swap(a[static_cast<size_t>(col * n + c)], a[static_cast<size_t>(pivot * n + c)]);
                std::swap(inv[static_cast<size_t>(col * n + c)], inv[static_cast<size_t>(pivot * n + c)]);
            }
        const double d = 1.0 / a[static_cast<size_t>(col * n + col)];
        for (int c = 0; c < n; ++c)
        {
            a[static_cast<size_t>(col * n + c)] *= d;
            inv[static_cast<size_t>(col * n + c)] *= d;
        }
        for (int r = 0; r < n; ++r)
        {
            if (r == col)
                continue;
            const double f = a[static_cast<size_t>(r * n + col)];
            if (f == 0.0)
                continue;
            for (int c = 0; c < n; ++c)
            {
                a[static_cast<size_t>(r * n + c)] -= f * a[static_cast<size_t>(col * n + c)];
                inv[static_cast<size_t>(r * n + c)] -= f * inv[static_cast<size_t>(col * n + c)];
            }
        }
    }
    std::copy(inv.begin(), inv.begin() + n * n, a.begin());
    return true;
}

constexpr double kProtoDb = 6.0;


constexpr double kLambda = 1e-5;
constexpr double kRefineTolerance = 0.003; // relative (T60) error at the design points

} // namespace

void GeqDesigner::prepare(double sampleRate)
{
    fs_ = sampleRate;
    const double third = std::pow(2.0, 1.0 / 3.0);
    const double bellQ = std::sqrt(third) / (third - 1.0) * qFactor;

    numBands_ = 0;
    auto addBand = [&](Kind kind, double f, double q) {
        kinds_[static_cast<size_t>(numBands_)] = kind;
        freqs_[static_cast<size_t>(numBands_)] = f;
        qs_[static_cast<size_t>(numBands_)] = q;
        ++numBands_;
    };
    // Two low shelves keep the response controlled below the audio band.
    addBand(Kind::LowShelf, 8.0, 0.7071);
    addBand(Kind::LowShelf, 20.0, 0.7071);
    double topBell = 0.0;
    for (int n = -16; n <= 13; ++n)
    {
        const double fc = 1000.0 * std::pow(2.0, n / 3.0);
        if (fc > 0.4 * fs_)
            break;
        addBand(Kind::Bell, fc, bellQ);
        topBell = fc;
    }
    if (topBell * 1.25 < 0.4 * fs_)
        addBand(Kind::HighShelf, topBell * 1.25, 0.7071);

    numPoints_ = 0;
    const double step = std::pow(2.0, 1.0 / 6.0);
    // Design points from ~4 Hz: the region below the lowest band must be
    // controlled too, or a fit can exceed unity there (infrasonic runaway).
    for (double f = 1000.0 * std::pow(2.0, -24.0 / 3.0); f < 0.46 * fs_ && numPoints_ < kMaxPoints; f *= step)
        points_[static_cast<size_t>(numPoints_++)] = f;

    warp_.assign(static_cast<size_t>(numBands_ * numPoints_), 0.0);
    for (int k = 0; k < numBands_; ++k)
        for (int m = 0; m < numPoints_; ++m)
            warp_[static_cast<size_t>(k * numPoints_ + m)] =
                response::warpRatio(points_[static_cast<size_t>(m)], freqs_[static_cast<size_t>(k)], fs_);

    buildInteraction();
    shape_.fill(0.0);
    shapeSolution_.fill(0.0);
    pinvShape_.assign(static_cast<size_t>((numBands_ + 1) * numPoints_), 0.0);
    scratchPinv_.assign(pinvShape_.size(), 0.0);
    normal_.assign(static_cast<size_t>((numBands_ + 1) * (numBands_ + 1)), 0.0);
    normalInv_.assign(normal_.size(), 0.0);
}

namespace {

inline double kindPow(GeqDesigner::Kind kind, double w, double q, double gainDb)
{
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w2 = w * w;
    switch (kind)
    {
    case GeqDesigner::Kind::Bell:
    {
        const double re = 1.0 - w2;
        const double n = w * A / q;
        const double d = w / (A * q);
        return (re * re + n * n) / (re * re + d * d);
    }
    case GeqDesigner::Kind::LowShelf:
    {
        const double im2 = w2 * A / (q * q);
        const double a = A - w2;
        const double b = 1.0 - A * w2;
        return A * A * (a * a + im2) / (b * b + im2);
    }
    case GeqDesigner::Kind::HighShelf:
    {
        const double im2 = w2 * A / (q * q);
        const double a = 1.0 - A * w2;
        const double b = A - w2;
        return A * A * (a * a + im2) / (b * b + im2);
    }
    }
    return 1.0;
}

} // namespace

double GeqDesigner::bandPowAt(int k, double gainDb, double freq) const
{
    const double w = response::warpRatio(freq, freqs_[static_cast<size_t>(k)], fs_);
    return kindPow(kinds_[static_cast<size_t>(k)], w, qs_[static_cast<size_t>(k)], gainDb);
}

void GeqDesigner::buildInteraction()
{
    const int K = numBands_ + 1;
    const int M = numPoints_;
    B_.assign(static_cast<size_t>(M * K), 0.0);
    for (int m = 0; m < M; ++m)
    {
        for (int k = 0; k < numBands_; ++k)
        {
            const double w = warp_[static_cast<size_t>(k * M + m)];
            const double p = kindPow(kinds_[static_cast<size_t>(k)], w, qs_[static_cast<size_t>(k)], kProtoDb);
            B_[static_cast<size_t>(m * K + k)] = 10.0 * std::log10(p) / kProtoDb;
        }
        B_[static_cast<size_t>(m * K + numBands_)] = 1.0;
    }
}

void GeqDesigner::buildWeightedPinv(const double* weights, std::vector<double>& pinv) const
{
    const int K = numBands_ + 1;
    const int M = numPoints_;
    std::vector<double>& N = normal_;
    std::fill(N.begin(), N.end(), 0.0);
    double w2[kMaxPoints];
    for (int m = 0; m < M; ++m)
        w2[m] = weights[m] * weights[m];
    for (int i = 0; i < K; ++i)
        for (int j = i; j < K; ++j)
        {
            double s = 0.0;
            for (int m = 0; m < M; ++m)
                s += B_[static_cast<size_t>(m * K + i)] * w2[m] * B_[static_cast<size_t>(m * K + j)];
            N[static_cast<size_t>(i * K + j)] = s;
            N[static_cast<size_t>(j * K + i)] = s;
        }
    for (int i = 0; i < numBands_; ++i)
        N[static_cast<size_t>(i * K + i)] += kLambda * M;
    invert(N, normalInv_, K);
    for (int i = 0; i < K; ++i)
        for (int m = 0; m < M; ++m)
        {
            double s = 0.0;
            for (int j = 0; j < K; ++j)
                s += N[static_cast<size_t>(i * K + j)] * B_[static_cast<size_t>(m * K + j)];
            pinv[static_cast<size_t>(i * M + m)] = s * w2[m];
        }
}

void GeqDesigner::applyPinv(const std::vector<double>& pinv, const double* t, double* gains, double& bb) const
{
    const int M = numPoints_;
    for (int k = 0; k <= numBands_; ++k)
    {
        const double* row = &pinv[static_cast<size_t>(k * M)];
        double s = 0.0;
        for (int m = 0; m < M; ++m)
            s += row[m] * t[m];
        if (k < numBands_)
            gains[k] = clamp(gains[k] + s, kMinBandDb, kMaxBandDb);
        else
            bb += s;
    }
}

namespace {

// 10*log10(p), with a series expansion for p close to 1 (the common case for
// bands far from their centre), which is both faster and more precise there.
inline double powToDb(double p)
{
    constexpr double kDbPerNeper = 4.342944819032518;
    const double x = p - 1.0;
    if (std::fabs(x) < 0.05)
        return kDbPerNeper * x * (1.0 - x * (0.5 - x * (1.0 / 3.0 - x * (0.25 - x * 0.2))));
    return kDbPerNeper * std::log(p);
}

} // namespace

void GeqDesigner::realisedAtPoints(const double* gains, double bb, double* out) const
{
    const int M = numPoints_;
    for (int m = 0; m < M; ++m)
        out[m] = bb;
    for (int k = 0; k < numBands_; ++k)
    {
        const double g = gains[k];
        if (std::fabs(g) < 1e-12)
            continue;
        const double A = std::pow(10.0, g / 40.0);
        const double q = qs_[static_cast<size_t>(k)];
        const double* w = &warp_[static_cast<size_t>(k * M)];
        switch (kinds_[static_cast<size_t>(k)])
        {
        case Kind::Bell:
        {
            const double nq = A / q, dq = 1.0 / (A * q);
            for (int m = 0; m < M; ++m)
            {
                const double w2 = w[m] * w[m];
                const double re2 = (1.0 - w2) * (1.0 - w2);
                out[m] += powToDb((re2 + w2 * nq * nq) / (re2 + w2 * dq * dq));
            }
            break;
        }
        case Kind::LowShelf:
        case Kind::HighShelf:
            for (int m = 0; m < M; ++m)
                out[m] += powToDb(kindPow(kinds_[static_cast<size_t>(k)], w[m], q, g));
            break;
        }
    }
}

void GeqDesigner::designLines(const double* delays, int n, double* gains, double* broadband) const
{
    constexpr int kRefs = 4;
    double dmin = delays[0], dmax = delays[0];
    for (int i = 1; i < n; ++i)
    {
        dmin = std::min(dmin, delays[i]);
        dmax = std::max(dmax, delays[i]);
    }
    const int refs = dmax > dmin * 1.05 ? kRefs : 1;
    double refGains[kRefs][kMaxBands];
    double refBb[kRefs];
    double refDelay[kRefs];
    for (int r = 0; r < refs; ++r)
    {
        refDelay[r] = refs > 1 ? gerp(dmin, dmax, double(r) / (refs - 1)) : dmin;
        design(refDelay[r], refGains[r], refBb[r]);
        for (int k = 0; k < numBands_; ++k)
            refGains[r][k] /= refDelay[r];
        refBb[r] /= refDelay[r];
    }
    for (int i = 0; i < n; ++i)
    {
        double* g = gains + static_cast<size_t>(i) * kMaxBands;
        const double d = delays[i];
        int r0 = 0;
        double t = 0.0;
        if (refs > 1)
        {
            const double u = std::log(d / dmin) / std::log(dmax / dmin) * (refs - 1);
            r0 = clamp(static_cast<int>(u), 0, refs - 2);
            t = clamp(u - r0, 0.0, 1.0);
        }
        const int r1 = std::min(r0 + 1, refs - 1);
        for (int k = 0; k < numBands_; ++k)
            g[k] = d * (refGains[r0][k] + (refGains[r1][k] - refGains[r0][k]) * t);
        broadband[i] = d * (refBb[r0] + (refBb[r1] - refBb[r0]) * t);
    }
}

void GeqDesigner::setShape(const double* shapeDbPerSample)
{
    const int M = numPoints_;
    double maxAbs = 0.0;
    for (int m = 0; m < M; ++m)
    {
        shape_[static_cast<size_t>(m)] = std::min(shapeDbPerSample[m], 0.0);
        maxAbs = std::max(maxAbs, std::fabs(shape_[static_cast<size_t>(m)]));
    }
    shapeSolution_.fill(0.0);
    if (maxAbs < 1e-15)
    {
        pinvShape_.assign(pinvShape_.size(), 0.0);
        return;
    }
    double weights[kMaxPoints];
    double sumW2 = 0.0;
    for (int m = 0; m < M; ++m)
    {
        weights[m] = 1.0 / std::max(std::fabs(shape_[static_cast<size_t>(m)]), 1e-3 * maxAbs);
        sumW2 += weights[m] * weights[m];
    }
    const double norm = std::sqrt(M / sumW2);
    for (int m = 0; m < M; ++m)
        weights[m] *= norm;
    buildWeightedPinv(weights, pinvShape_);
    double bb = 0.0;
    applyPinv(pinvShape_, shape_.data(), shapeSolution_.data(), bb);
    shapeSolution_[static_cast<size_t>(numBands_)] = bb;
}

void GeqDesigner::design(double delaySamples, double* bandGainsDb, double& broadbandDb, int refinePasses) const
{
    const int M = numPoints_;
    for (int k = 0; k < numBands_; ++k)
        bandGainsDb[k] = delaySamples * shapeSolution_[static_cast<size_t>(k)];
    broadbandDb = delaySamples * shapeSolution_[static_cast<size_t>(numBands_)];
    if (broadbandDb == 0.0 && refinePasses == 0)
        return;

    double target[kMaxPoints];
    bool clipped = false;
    for (int m = 0; m < M; ++m)
    {
        const double t = delaySamples * shape_[static_cast<size_t>(m)];
        target[m] = std::max(t, kMinTargetDb);
        clipped |= t < kMinTargetDb;
    }
    // A clipped target has a different shape: fit it with its own weights.
    if (clipped)
    {
        designTarget(target, bandGainsDb, broadbandDb, refinePasses);
        return;
    }
    refine(pinvShape_, target, bandGainsDb, broadbandDb, refinePasses);
}

void GeqDesigner::designTarget(const double* targetDb, double* bandGainsDb, double& broadbandDb, int refinePasses) const
{
    const int M = numPoints_;
    double maxAbs = 0.0;
    for (int m = 0; m < M; ++m)
        maxAbs = std::max(maxAbs, std::fabs(targetDb[m]));
    double weights[kMaxPoints];
    double sumW2 = 0.0;
    for (int m = 0; m < M; ++m)
    {
        weights[m] = 1.0 / std::max(std::fabs(targetDb[m]), 1e-3 * std::max(maxAbs, 1e-12));
        sumW2 += weights[m] * weights[m];
    }
    const double norm = std::sqrt(M / sumW2);
    for (int m = 0; m < M; ++m)
        weights[m] *= norm;
    std::vector<double>& pinv = scratchPinv_;
    buildWeightedPinv(weights, pinv);
    for (int k = 0; k < numBands_; ++k)
        bandGainsDb[k] = 0.0;
    broadbandDb = 0.0;
    applyPinv(pinv, targetDb, bandGainsDb, broadbandDb);
    refine(pinv, targetDb, bandGainsDb, broadbandDb, refinePasses);
}

void GeqDesigner::refine(const std::vector<double>& pinv, const double* targetDb, double* gains, double& bb,
                         int maxPasses) const
{
    const int M = numPoints_;
    double realised[kMaxPoints];
    double err[kMaxPoints];
    double bestGains[kMaxBands];
    double bestBb = bb;
    double best = 1e300;
    for (int pass = 0; pass <= maxPasses; ++pass)
    {
        realisedAtPoints(gains, bb, realised);
        double worst = 0.0;
        for (int m = 0; m < M; ++m)
        {
            err[m] = targetDb[m] - realised[m];
            worst = std::max(worst, std::fabs(err[m]) / std::max(std::fabs(targetDb[m]), 1e-9));
        }
        if (worst < best)
        {
            best = worst;
            std::copy(gains, gains + numBands_, bestGains);
            bestBb = bb;
        }
        else if (worst > 2.0 * best)
        {
            break; // diverging (steep target far outside the linear range)
        }
        if (worst < kRefineTolerance || pass == maxPasses)
            break;
        applyPinv(pinv, err, gains, bb);
    }
    std::copy(bestGains, bestGains + numBands_, gains);
    bb = bestBb;

    // Safety: every frequency must keep at least half of the smallest
    // attenuation the loop needs, so a poor fit can only shorten the decay
    // a little and never makes the loop unstable. Small ripple is allowed.
    realisedAtPoints(gains, bb, realised);
    double ceiling = -1e300;
    for (int m = 0; m < M; ++m)
        ceiling = std::max(ceiling, targetDb[m]);
    const double limit = 0.5 * std::min(ceiling, 0.0);
    double over = -1e300;
    for (int m = 0; m < M; ++m)
        over = std::max(over, realised[m] - limit);
    // Off-grid checks: sub-audio, between the lowest points, top octave.
    auto checkOff = [&](double f) { over = std::max(over, realisedDbAt(gains, bb, f) - limit); };
    for (double f : {0.5, 1.0, 2.0, 3.0, 0.475 * fs_, 0.485 * fs_, 0.495 * fs_})
        checkOff(f);
    for (int m = 0; m + 1 < M; ++m)
    {
        const double mid = std::sqrt(points_[static_cast<size_t>(m)] * points_[static_cast<size_t>(m + 1)]);
        if (mid < 30.0)
            checkOff(mid);
    }
    if (over > 0.0)
        bb -= over;
}

double GeqDesigner::realisedDbAt(const double* bandGainsDb, double broadbandDb, double freq) const
{
    double db = broadbandDb;
    for (int k = 0; k < numBands_; ++k)
        db += 10.0 * std::log10(bandPowAt(k, bandGainsDb[k], freq));
    return db;
}

} // namespace aurum::dsp
