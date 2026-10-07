#pragma once

#include <array>

namespace substrike::dsp {

// Polyphase IIR halfband filters: two chains of first-order allpasses in z^-2,
// one per phase, after L. de Soras' "hiir" library. The coefficients come from
// its elliptic design procedure (PolyphaseIir2Designer), computed offline and
// checked against the transfer function
//
//   H(z) = 1/2 (A0(z^2) + z^-1 A1(z^2))
//
// No latency, only phase, which suits a kick: nothing in front of a transient.
class Halfband
{
public:
    static constexpr int kMaxCoefs = 8;

    void init(const double* coefs, int count)
    {
        count_ = count;
        for (int i = 0; i < count; ++i)
            c_[static_cast<size_t>(i)] = coefs[i];
        reset();
    }

    void reset()
    {
        x_.fill(0.0);
        y_.fill(0.0);
    }

    // One sample in at the low rate, two out at the high rate.
    void up(double in, double& out0, double& out1)
    {
        out0 = path(0, in);
        out1 = path(1, in);
    }

    // Two samples in at the high rate, one out at the low rate.
    double down(double in0, double in1) { return 0.5 * (path(0, in1) + path(1, in0)); }

private:
    // Phase p runs the coefficients p, p + 2, p + 4, ...
    double path(int p, double v)
    {
        for (int i = p; i < count_; i += 2)
        {
            const size_t k = static_cast<size_t>(i);
            const double t = c_[k] * (v - y_[k]) + x_[k];
            x_[k] = v;
            y_[k] = t;
            v = t;
        }
        return v;
    }

    std::array<double, kMaxCoefs> c_{}, x_{}, y_{};
    int count_ = 0;
};

// 1x, 2x or 4x for one channel. The first stage is the steep one: flat to 0.42
// of the base rate, 99 dB down from 0.58. The second, at twice that rate, only
// has to clear what the first left, and is cheaper for it (90 dB).
class Oversampler
{
public:
    static constexpr int kMaxFactor = 4;

    Oversampler()
    {
        static const double kStage1[8] = {0.04063346092419326, 0.1505051290226746, 0.30075705599187408,
                                          0.46077450496145061, 0.6095243148961883, 0.73850384111885725,
                                          0.84922381039206607, 0.9497427837050002};
        static const double kStage2[5] = {0.050857024439668115, 0.18898752479217637, 0.38248720173098638,
                                          0.6052535917518781, 0.85542250996786606};
        up1_.init(kStage1, 8);
        down1_.init(kStage1, 8);
        up2_.init(kStage2, 5);
        down2_.init(kStage2, 5);
    }

    void setFactor(int factor)
    {
        factor_ = factor >= 4 ? 4 : factor >= 2 ? 2 : 1;
        reset();
    }
    int factor() const { return factor_; }

    void reset()
    {
        up1_.reset();
        up2_.reset();
        down1_.reset();
        down2_.reset();
    }

    // n samples in, n * factor() out.
    void up(const float* in, float* out, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            if (factor_ == 1)
            {
                out[i] = in[i];
                continue;
            }
            double a, b;
            up1_.up(in[i], a, b);
            if (factor_ == 2)
            {
                out[2 * i] = static_cast<float>(a);
                out[2 * i + 1] = static_cast<float>(b);
                continue;
            }
            double a0, a1, b0, b1;
            up2_.up(a, a0, a1);
            up2_.up(b, b0, b1);
            out[4 * i] = static_cast<float>(a0);
            out[4 * i + 1] = static_cast<float>(a1);
            out[4 * i + 2] = static_cast<float>(b0);
            out[4 * i + 3] = static_cast<float>(b1);
        }
    }

    // n * factor() samples in, n out.
    void down(const float* in, float* out, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            if (factor_ == 1)
                out[i] = in[i];
            else if (factor_ == 2)
                out[i] = static_cast<float>(down1_.down(in[2 * i], in[2 * i + 1]));
            else
            {
                const double a = down2_.down(in[4 * i], in[4 * i + 1]);
                const double b = down2_.down(in[4 * i + 2], in[4 * i + 3]);
                out[i] = static_cast<float>(down1_.down(a, b));
            }
        }
    }

private:
    Halfband up1_, up2_, down1_, down2_;
    int factor_ = 1;
};

} // namespace substrike::dsp
