#include "Effects.h"

#include <algorithm>
#include <cmath>

namespace substrike::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLn1000 = 6.907755278982137;
// Every value and the mix glide over this long.
constexpr double kGlideMs = 5.0;

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

// Samples for a time in milliseconds, at least one.
double samples(double ms, double rate) { return std::max(ms * 0.001 * rate, 1.0); }

// A one-pole follower coefficient: the state covers 1 - 1/e of a step in `ms`.
double follower(double ms, double rate) { return std::exp(-1.0 / samples(ms, rate)); }

} // namespace

bool slotValueIsChoice(SlotType t, int i)
{
    switch (t)
    {
    case SlotType::Distortion:
    case SlotType::Filter:
    case SlotType::Gate:
    case SlotType::Ring: return i == 0;
    case SlotType::Delay: return i == 1;
    case SlotType::Warp: return i == 1 || i == 4;
    case SlotType::Smear: return i == 2;
    case SlotType::Utility: return i == 1 || i == 2;
    default: return false;
    }
}

bool slotIsOversampled(SlotType t)
{
    return t == SlotType::Distortion || t == SlotType::Clipper || t == SlotType::Wavefolder ||
           t == SlotType::Bitcrush;
}

// ----------------------------------------------------------------- Distortion

void DistortionFx::prepare(double sampleRate)
{
    rate_ = sampleRate;
    for (int c = 0; c < 2; ++c)
    {
        stage_[c].prepare(sampleRate);
        // Low enough to take an offset out, low enough to leave a 30 Hz sub
        // nearly alone (14 degrees of phase).
        dc_[c].setCutoff(5.0f, static_cast<float>(sampleRate));
    }
    last_.fill(-1.0);
    reset();
}

void DistortionFx::reset()
{
    for (int c = 0; c < 2; ++c)
    {
        stage_[c].reset();
        tone_[c].reset();
        dc_[c].reset();
    }
}

void DistortionFx::set(const double* v)
{
    // setParams measures the model's curve to match its level, so it runs
    // only when something it depends on moved.
    if (v[0] != last_[0] || v[1] != last_[1] || v[2] != last_[2])
    {
        const int model = static_cast<int>(std::lround(v[0]));
        for (int c = 0; c < 2; ++c)
            stage_[c].setParams(model, static_cast<float>(v[1] / 100.0), static_cast<float>(v[2] / 100.0), 1.0f);
    }
    if (v[3] != last_[3])
        for (int c = 0; c < 2; ++c)
            tone_[c].setup(v[3], 0.7071067811865476, rate_);
    for (int i = 0; i < 4; ++i)
        last_[static_cast<size_t>(i)] = v[i];
    out_ = dbToGain(v[4]);
}

void DistortionFx::process(float* l, float* r, int n)
{
    float* ch[2] = {l, r};
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
        {
            const float y = stage_[c].tick(ch[c][i]);
            ch[c][i] = static_cast<float>(dc_[c].tick(static_cast<float>(tone_[c].lowPass(y))) * out_);
        }
}

// -------------------------------------------------------------------- Clipper

// Linear up to 1 - knee, flat from 1 + knee, and a parabola between that meets
// both with matching slope. Knee 0 is a hard clip.
double ClipperFx::shape(double u, double knee)
{
    const double a = std::fabs(u);
    double y;
    if (knee <= 0.0)
        y = std::min(a, 1.0);
    else if (a <= 1.0 - knee)
        y = a;
    else if (a >= 1.0 + knee)
        y = 1.0;
    else
    {
        const double d = 1.0 + knee - a;
        y = 1.0 - d * d / (4.0 * knee);
    }
    return u < 0.0 ? -y : y;
}

void ClipperFx::set(const double* v)
{
    gain_ = dbToGain(v[0]);
    knee_ = std::clamp(v[1] / 100.0, 0.0, 1.0);
    ceiling_ = dbToGain(v[2]);
}

void ClipperFx::process(float* l, float* r, int n)
{
    const double in = gain_ / ceiling_;
    for (int i = 0; i < n; ++i)
    {
        l[i] = static_cast<float>(ceiling_ * shape(l[i] * in, knee_));
        r[i] = static_cast<float>(ceiling_ * shape(r[i] * in, knee_));
    }
}

// ----------------------------------------------------------------- Wavefolder

void WavefolderFx::prepare(double sampleRate)
{
    for (OnePoleHp& d : dc_)
        d.setCutoff(5.0f, static_cast<float>(sampleRate));
    reset();
}

void WavefolderFx::reset()
{
    for (OnePoleHp& d : dc_)
        d.reset();
}

void WavefolderFx::set(const double* v)
{
    gain_ = dbToGain(v[0]);
    bias_ = v[1] / 100.0;
    shape_ = std::clamp(v[2] / 100.0, 0.0, 1.0);
    out_ = dbToGain(v[3]);
}

void WavefolderFx::process(float* l, float* r, int n)
{
    float* ch[2] = {l, r};
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
        {
            // A sine folder, and the triangle folder it becomes when the
            // sine is straightened out: asin(sin(x)) folds without rounding.
            const double s = std::sin(0.5 * kPi * (gain_ * ch[c][i] + bias_));
            const double t = (2.0 / kPi) * std::asin(std::clamp(s, -1.0, 1.0));
            const double y = s + shape_ * (t - s);
            ch[c][i] = static_cast<float>(dc_[c].tick(static_cast<float>(y)) * out_);
        }
}

// ------------------------------------------------------------------- Bitcrush

void BitcrushFx::prepare(double sampleRate)
{
    rate_ = sampleRate;
    reset();
}

void BitcrushFx::reset()
{
    phase_ = 1.0;
    held_[0] = held_[1] = 0.0f;
}

void BitcrushFx::set(const double* v)
{
    step_ = std::exp2(1.0 - std::clamp(v[0], 1.0, 16.0));
    holding_ = v[1] < kRateOff;
    hold_ = v[1] / rate_;
    out_ = dbToGain(v[2]);
}

void BitcrushFx::process(float* l, float* r, int n)
{
    for (int i = 0; i < n; ++i)
    {
        float x[2] = {l[i], r[i]};
        if (holding_)
        {
            phase_ += hold_;
            if (phase_ >= 1.0)
            {
                phase_ -= std::floor(phase_);
                held_[0] = x[0];
                held_[1] = x[1];
            }
            x[0] = held_[0];
            x[1] = held_[1];
        }
        l[i] = static_cast<float>(step_ * std::round(x[0] / step_) * out_);
        r[i] = static_cast<float>(step_ * std::round(x[1] / step_) * out_);
    }
}

// --------------------------------------------------------------------- Filter

void FilterFx::prepare(double sampleRate)
{
    rate_ = sampleRate;
    reset();
}

void FilterFx::reset()
{
    for (int c = 0; c < 2; ++c)
    {
        a_[c].reset();
        b_[c].reset();
    }
    env_ = 0.0;
    setFor_ = -1.0;
}

void FilterFx::set(const double* v)
{
    const int mode = std::clamp(static_cast<int>(std::lround(v[0])), 0, 6);
    const double q = resonanceToQ(v[2] / 100.0);
    const double peak = std::pow(10.0, v[5] / 40.0);
    if (mode != mode_ || q != q_ || peak != peak_)
        setFor_ = -1.0;
    mode_ = mode;
    q_ = q;
    peak_ = peak;
    cutoff_ = v[1];
    envOct_ = v[3];
    envFall_ = std::exp(-kLn1000 / samples(v[4], rate_));
}

void FilterFx::setup(double cutoff)
{
    setFor_ = cutoff;
    const bool steep = mode_ == 1 || mode_ == 3;
    // A bell's band output is scaled by Q * A, after A. Simper's SVF notes.
    const double qa = mode_ == 6 ? q_ * peak_ : steep ? 0.7071067811865476 : q_;
    for (int c = 0; c < 2; ++c)
    {
        a_[c].setup(cutoff, qa, rate_);
        if (steep)
            b_[c].setup(cutoff, q_, rate_);
    }
}

void FilterFx::process(float* l, float* r, int n)
{
    float* ch[2] = {l, r};
    for (int i = 0; i < n; ++i)
    {
        if (envOct_ != 0.0 && env_ > 1e-5)
        {
            setup(cutoff_ * std::exp2(envOct_ * env_));
            env_ *= envFall_;
        }
        else if (setFor_ != cutoff_)
            setup(cutoff_);
        for (int c = 0; c < 2; ++c)
        {
            const double x = ch[c][i];
            double lo, bp, hi, y;
            a_[c].tick(x, lo, bp, hi);
            switch (mode_)
            {
            case 0: y = lo; break;
            case 1: y = b_[c].lowPass(lo); break;
            case 2: y = hi; break;
            case 3: y = b_[c].highPass(hi); break;
            case 4: y = a_[c].k() * bp; break; // unity at the peak
            case 5: y = x - a_[c].k() * bp; break;
            default: y = x + a_[c].k() * (peak_ * peak_ - 1.0) * bp; break;
            }
            ch[c][i] = static_cast<float>(y);
        }
    }
}

// ------------------------------------------------------------------------- EQ

namespace {
struct Rbj
{
    double a, w, cw, alpha, sq;
    Rbj(double f, double db, double q, double rate)
    {
        a = std::pow(10.0, db / 40.0);
        w = 2.0 * kPi * std::clamp(f, 10.0, 0.45 * rate) / rate;
        cw = std::cos(w);
        alpha = std::sin(w) / (2.0 * q);
        sq = 2.0 * std::sqrt(a) * alpha;
    }
};

void normalise(Biquad& b, double b0, double b1, double b2, double a0, double a1, double a2)
{
    b.b0 = b0 / a0;
    b.b1 = b1 / a0;
    b.b2 = b2 / a0;
    b.a1 = a1 / a0;
    b.a2 = a2 / a0;
}
} // namespace

void Biquad::lowShelf(double f, double db, double q, double rate)
{
    const Rbj c(f, db, q, rate);
    const double A = c.a;
    normalise(*this, A * ((A + 1) - (A - 1) * c.cw + c.sq), 2 * A * ((A - 1) - (A + 1) * c.cw),
              A * ((A + 1) - (A - 1) * c.cw - c.sq), (A + 1) + (A - 1) * c.cw + c.sq,
              -2 * ((A - 1) + (A + 1) * c.cw), (A + 1) + (A - 1) * c.cw - c.sq);
}

void Biquad::highShelf(double f, double db, double q, double rate)
{
    const Rbj c(f, db, q, rate);
    const double A = c.a;
    normalise(*this, A * ((A + 1) + (A - 1) * c.cw + c.sq), -2 * A * ((A - 1) + (A + 1) * c.cw),
              A * ((A + 1) + (A - 1) * c.cw - c.sq), (A + 1) - (A - 1) * c.cw + c.sq,
              2 * ((A - 1) - (A + 1) * c.cw), (A + 1) - (A - 1) * c.cw - c.sq);
}

void Biquad::peak(double f, double db, double q, double rate)
{
    const Rbj c(f, db, q, rate);
    normalise(*this, 1 + c.alpha * c.a, -2 * c.cw, 1 - c.alpha * c.a, 1 + c.alpha / c.a, -2 * c.cw,
              1 - c.alpha / c.a);
}

void CombFx::prepare(double sampleRate)
{
    rate_ = sampleRate;
    last_.fill(1e9);
    reset();
}

void CombFx::reset()
{
    for (auto& ch : bq_)
        for (Biquad& b : ch)
            b.reset();
}

void CombFx::set(const double* v)
{
    bool same = true;
    for (int i = 0; i < 6; ++i)
        same &= v[i] == last_[static_cast<size_t>(i)];
    if (same)
        return;
    for (int i = 0; i < 6; ++i)
        last_[static_cast<size_t>(i)] = v[i];
    const int count = std::clamp(static_cast<int>(std::lround(v[2])), 1, kMaxBands);
    const double top = 0.45 * rate_, taper = std::clamp(v[5] / 100.0, -1.0, 1.0);
    const double width = std::max(v[1] * v[4] / 100.0, 0.1); // Hz
    const int old = bands_;
    bands_ = 0;
    for (int k = 0; k < count; ++k)
    {
        const double f = v[0] + k * v[1];
        if (f >= top)
            break;
        // Where this band sits along the comb, 0 at the first, 1 at the last.
        const double at = count > 1 ? static_cast<double>(k) / (count - 1) : 0.0;
        const double share = taper >= 0.0 ? 1.0 - taper * at : 1.0 + taper * (1.0 - at);
        for (auto& ch : bq_)
            ch[k].peak(f, v[3] * share, f / width, rate_);
        ++bands_;
    }
    // A band that comes back starts from rest.
    for (int k = old; k < bands_; ++k)
        for (auto& ch : bq_)
            ch[k].reset();
}

void CombFx::process(float* l, float* r, int n)
{
    float* ch[2] = {l, r};
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
        {
            double x = ch[c][i];
            for (int k = 0; k < bands_; ++k)
                x = bq_[c][k].tick(x);
            ch[c][i] = static_cast<float>(x);
        }
}

void EqFx::prepare(double sampleRate)
{
    rate_ = sampleRate;
    last_.fill(1e9);
    reset();
}

void EqFx::reset()
{
    for (auto& ch : bq_)
        for (Biquad& b : ch)
            b.reset();
}

void EqFx::set(const double* v)
{
    bool same = true;
    for (int i = 0; i < 6; ++i)
        same &= v[i] == last_[static_cast<size_t>(i)];
    if (same)
        return;
    for (int i = 0; i < 6; ++i)
        last_[static_cast<size_t>(i)] = v[i];
    for (auto& ch : bq_)
    {
        ch[0].lowShelf(100.0, v[0], 0.7071067811865476, rate_);
        ch[1].peak(v[2], v[1], v[3], rate_);
        ch[2].highShelf(6000.0, v[4], 0.7071067811865476, rate_);
        ch[3].lowShelf(1000.0, -0.5 * v[5], 0.5, rate_);
        ch[4].highShelf(1000.0, 0.5 * v[5], 0.5, rate_);
    }
}

void EqFx::process(float* l, float* r, int n)
{
    float* ch[2] = {l, r};
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
        {
            double x = ch[c][i];
            for (Biquad& b : bq_[c])
                x = b.tick(x);
            ch[c][i] = static_cast<float>(x);
        }
}

// ----------------------------------------------------------------- Compressor

void CompressorFx::prepare(double sampleRate)
{
    rate_ = sampleRate;
    reset();
}

void CompressorFx::set(const double* v)
{
    thr_ = v[0];
    slope_ = 1.0 - 1.0 / std::max(v[1], 1.0);
    att_ = follower(v[2], rate_);
    rel_ = follower(v[3], rate_);
    knee_ = std::max(v[4], 0.0);
    makeup_ = v[5];
}

void CompressorFx::process(float* l, float* r, int n)
{
    for (int i = 0; i < n; ++i)
    {
        const double peak = std::max(std::fabs(l[i]), std::fabs(r[i]));
        const double over = 20.0 * std::log10(peak + 1e-12) - thr_;
        double gr = 0.0;
        if (knee_ > 0.0 && std::fabs(over) <= 0.5 * knee_)
        {
            const double d = over + 0.5 * knee_;
            gr = slope_ * d * d / (2.0 * knee_);
        }
        else if (over > 0.0)
            gr = slope_ * over;
        env_ = gr + (gr > env_ ? att_ : rel_) * (env_ - gr);
        const double g = dbToGain(makeup_ - env_);
        l[i] = static_cast<float>(l[i] * g);
        r[i] = static_cast<float>(r[i] * g);
    }
}

// ------------------------------------------------------------------ Transient

void TransientFx::prepare(double sampleRate)
{
    rate_ = sampleRate;
    reset();
}

void TransientFx::reset()
{
    eFastA_ = eSlowA_ = eFastR_ = eSlowR_ = 0.0;
    gainDb_ = 0.0;
}

void TransientFx::set(const double* v)
{
    attack_ = v[0] / 100.0;
    sustain_ = v[1] / 100.0;
    const double speed = std::max(v[2], 0.1);
    fastAtk_ = follower(0.1, rate_);
    slowAtk_ = follower(speed, rate_);
    atkRel_ = follower(50.0, rate_);
    relAtk_ = follower(0.1, rate_);
    fastRel_ = follower(10.0, rate_);
    slowRel_ = follower(10.0 * speed, rate_);
    out_ = dbToGain(v[3]);
    gainGlide_ = 1.0 - follower(0.3, rate_);
}

void TransientFx::process(float* l, float* r, int n)
{
    auto follow = [](double& e, double x, double up, double down) { e = x + (x > e ? up : down) * (e - x); };
    for (int i = 0; i < n; ++i)
    {
        const double x = std::max(std::fabs(l[i]), std::fabs(r[i]));
        follow(eFastA_, x, fastAtk_, atkRel_);
        follow(eSlowA_, x, slowAtk_, atkRel_);
        follow(eFastR_, x, relAtk_, fastRel_);
        follow(eSlowR_, x, relAtk_, slowRel_);
        constexpr double eps = 1e-6;
        // Each contrast is taken up to 12 dB: from silence the slow follower
        // starts at nothing and the onset ratio is huge for the first
        // samples, which would turn an attack into a spike. So Attack and
        // Sustain are a share of a 12 dB lift or cut, and the gain glides
        // over a third of a millisecond.
        const double onset = std::clamp(20.0 * std::log10((eFastA_ + eps) / (eSlowA_ + eps)), 0.0, 12.0);
        const double tail = std::clamp(20.0 * std::log10((eSlowR_ + eps) / (eFastR_ + eps)), 0.0, 12.0);
        const double db = attack_ * onset + sustain_ * tail;
        gainDb_ += gainGlide_ * (db - gainDb_);
        const double g = dbToGain(gainDb_) * out_;
        l[i] = static_cast<float>(l[i] * g);
        r[i] = static_cast<float>(r[i] * g);
    }
}

// ----------------------------------------------------------------------- Gate

void GateFx::prepare(double sampleRate)
{
    rate_ = sampleRate;
    // The level detector holds a peak for about 10 ms.
    detRelease_ = follower(10.0, rate_);
    reset();
}

void GateFx::reset()
{
    detector_ = 0.0;
    g_ = 0.0;
    hold_ = 0;
    opening_ = false;
}

void GateFx::set(const double* v)
{
    hitMode_ = std::lround(v[0]) == 1;
    thr_ = dbToGain(v[1]);
    attackStep_ = v[2] > 0.0 ? 1.0 / samples(v[2], rate_) : 1.0;
    holdSamples_ = static_cast<int>(v[3] * 0.001 * rate_);
    release_ = std::exp(-kLn1000 / samples(v[4], rate_));
    range_ = v[5] <= -79.95 ? 0.0 : dbToGain(v[5]);
}

void GateFx::hit()
{
    // In Gate mode the level decides alone.
    if (!hitMode_)
        return;
    opening_ = true;
    hold_ = holdSamples_;
}

void GateFx::process(float* l, float* r, int n)
{
    for (int i = 0; i < n; ++i)
    {
        if (hitMode_)
        {
            if (opening_)
            {
                g_ += attackStep_;
                if (g_ >= 1.0)
                {
                    g_ = 1.0;
                    opening_ = false;
                }
            }
            else if (hold_ > 0)
                --hold_;
            else
                g_ *= release_;
        }
        else
        {
            const double x = std::max(std::fabs(l[i]), std::fabs(r[i]));
            detector_ = std::max(x, detector_ * detRelease_);
            if (detector_ > thr_)
                hold_ = holdSamples_ + 1;
            if (hold_ > 0)
            {
                --hold_;
                g_ = std::min(1.0, g_ + attackStep_);
            }
            else
                g_ *= release_;
        }
        const double gain = range_ + (1.0 - range_) * g_;
        l[i] = static_cast<float>(l[i] * gain);
        r[i] = static_cast<float>(r[i] * gain);
    }
}

// -------------------------------------------------------------------- Limiter

void LimiterFx::prepare(double sampleRate)
{
    rate_ = sampleRate;
    holdSamples_ = static_cast<int>(0.025 * sampleRate);
    reset();
}

void LimiterFx::set(const double* v)
{
    in_ = dbToGain(v[0]);
    ceiling_ = dbToGain(v[1]);
    release_ = follower(v[2], rate_);
}

void LimiterFx::setFixed(double ceiling, double releaseMs)
{
    in_ = 1.0;
    ceiling_ = ceiling;
    release_ = follower(releaseMs, rate_);
}

void LimiterFx::process(float* l, float* r, int n)
{
    for (int i = 0; i < n; ++i)
    {
        const double a = l[i] * in_, b = r[i] * in_;
        const double peak = std::max(std::fabs(a), std::fabs(b));
        const double want = peak > ceiling_ ? ceiling_ / peak : 1.0;
        // Down at once, held for a period of the lowest kick, then back up
        // over the release: without the hold the gain would rise between
        // the peaks of every cycle and ride the waveform. Both sides alike,
        // so the image does not move.
        if (want <= gain_)
        {
            gain_ = want;
            hold_ = holdSamples_;
        }
        else if (hold_ > 0)
            --hold_;
        else
            gain_ = want + release_ * (gain_ - want);
        l[i] = static_cast<float>(a * gain_);
        r[i] = static_cast<float>(b * gain_);
    }
}

// ----------------------------------------------------------------------- Slot

Effect& Slot::effect(SlotType t)
{
    switch (t)
    {
    case SlotType::Distortion: return distortion_;
    case SlotType::Wavefolder: return wavefolder_;
    case SlotType::Bitcrush: return bitcrush_;
    case SlotType::Filter: return filter_;
    case SlotType::Eq: return eq_;
    case SlotType::Compressor: return compressor_;
    case SlotType::Transient: return transient_;
    case SlotType::Gate: return gate_;
    case SlotType::Reverb: return reverb_;
    case SlotType::Delay: return delay_;
    case SlotType::Warp: return warp_;
    case SlotType::Smear: return smear_;
    case SlotType::Ring: return ring_;
    case SlotType::Stereo: return stereo_;
    case SlotType::Utility: return utility_;
    case SlotType::Limiter: return limiter_;
    case SlotType::Comb: return comb_;
    case SlotType::Clipper:
    case SlotType::Off:
    default: return clipper_;
    }
}

void Slot::prepare(double sampleRate, int oversampling)
{
    rate_ = sampleRate;
    factor_ = oversampling;
    osL_.setFactor(oversampling);
    osR_.setFactor(oversampling);
    stepCoef_ = 1.0 - std::exp(-kStep / (kGlideMs * 0.001 * sampleRate));
    mixCoef_ = 1.0 - std::exp(-1.0 / (kGlideMs * 0.001 * sampleRate));
    size_t need = 0;
    for (int t = 1; t < kNumSlotTypes; ++t)
    {
        const SlotType type = static_cast<SlotType>(t);
        effect(type).prepare(slotIsOversampled(type) ? sampleRate * osL_.factor() : sampleRate);
        need = std::max(need, effect(type).memory(sampleRate));
    }
    // Only a new rate changes the size, and only activation brings one; a
    // new oversampling factor re-prepares on the audio thread and must not
    // allocate. (The time effects run at the base rate.)
    if (memory_.size() != need)
    {
        memory_.assign(need, 0.0f);
        memoryDirty_ = false;
    }
    for (int t = 1; t < kNumSlotTypes; ++t)
        if (effect(static_cast<SlotType>(t)).memory(sampleRate) > 0)
            effect(static_cast<SlotType>(t)).attach(memory_.data());
    reset();
}

void Slot::clearMemory()
{
    if (memoryDirty_)
        std::fill(memory_.begin(), memory_.end(), 0.0f);
    memoryDirty_ = false;
}

int Slot::silentHold() const
{
    return live_ ? const_cast<Slot*>(this)->effect(type_).silentHold() : 0;
}

void Slot::reset()
{
    clearMemory();
    for (int t = 1; t < kNumSlotTypes; ++t)
        effect(static_cast<SlotType>(t)).reset();
    osL_.reset();
    osR_.reset();
    xL_.reset();
    xR_.reset();
    xoverSetLow_ = xoverSetHigh_ = -1.0;
    mix_ = 0.0;
    keyRatio_ = 1.0;
    counter_ = 0;
    primed_ = false;
    live_ = false;
}

void Slot::glide(const SlotParams& p, const SlotEnv& e, bool snap)
{
    auto approach = [&](double& cur, double target) {
        if (snap)
            cur = target;
        else
        {
            cur += stepCoef_ * (target - cur);
            if (std::fabs(target - cur) <= 1e-9 * std::max(1.0, std::fabs(target)))
                cur = target;
        }
    };
    for (int i = 0; i < kSlotValues; ++i)
    {
        double& cur = cur_[static_cast<size_t>(i)];
        if (slotValueIsChoice(type_, i))
            cur = p.v[static_cast<size_t>(i)];
        else
            approach(cur, p.v[static_cast<size_t>(i)]);
    }
    approach(xoverLow_, e.xoverLow);
    approach(xoverHigh_, e.xoverHigh);
    if (band_ != Band::Full && (xoverLow_ != xoverSetLow_ || xoverHigh_ != xoverSetHigh_))
    {
        xoverSetLow_ = xoverLow_;
        xoverSetHigh_ = xoverHigh_;
        // The upper crossover stays at least half an octave above the lower.
        const double hi = std::max(xoverHigh_, xoverLow_ * 1.5);
        xL_.setup(xoverLow_, hi, rate_);
        xR_.setup(xoverLow_, hi, rate_);
    }
    effect(type_).setTempo(e.tempo);
    apply();
}

void Slot::apply()
{
    std::array<double, kSlotValues> v = cur_;
    if (keyRatio_ != 1.0)
    {
        auto tune = [&](int i) {
            double& f = v[static_cast<size_t>(i)];
            f = std::clamp(f * keyRatio_, 1.0, 0.45 * rate_);
        };
        if (type_ == SlotType::Filter || type_ == SlotType::Ring)
            tune(1);
        else if (type_ == SlotType::Eq)
            tune(2);
        else if (type_ == SlotType::Comb)
        {
            tune(0);
            tune(1);
        }
    }
    effect(type_).set(v.data());
}

void Slot::wake(const SlotParams& p, const SlotEnv& e)
{
    live_ = true;
    type_ = p.type;
    band_ = p.band;
    clearMemory();
    effect(type_).reset();
    osL_.reset();
    osR_.reset();
    xL_.reset();
    xR_.reset();
    xoverSetLow_ = xoverSetHigh_ = -1.0;
    glide(p, e, true);
}

void Slot::process(float* l, float* r, int n, const SlotParams& p, const SlotEnv& e, const int* hits, int hitCount,
                   const double* keyRatios)
{
    const double target = p.type == SlotType::Off || p.bypass ? 0.0 : std::clamp(p.mix, 0.0, 1.0);
    if (!live_)
    {
        if (target <= 0.0)
        {
            primed_ = true;
            counter_ = (counter_ + n) % kStep;
            return;
        }
        // Switched on in the middle of a sound, the slot fades in; on the
        // first hit after a reset it is simply there.
        const bool fresh = !primed_;
        wake(p, e);
        mix_ = fresh ? target : 0.0;
    }
    primed_ = true;
    // A new type starts clean and takes its values as they are. (Off keeps
    // the old effect running while it fades out.)
    if (p.type != SlotType::Off && p.type != type_)
    {
        type_ = p.type;
        clearMemory();
        effect(type_).reset();
        glide(p, e, true);
    }
    if (p.band != band_)
    {
        band_ = p.band;
        xL_.reset();
        xR_.reset();
        xoverSetLow_ = xoverSetHigh_ = -1.0;
        glide(p, e, true);
    }

    int i = 0, h = 0;
    while (i < n)
    {
        // A slot switched to Off keeps its old effect running while it fades
        // out, with the values that effect had: the letters it would glide
        // to now are an empty slot's, which mean nothing (or something wild)
        // to the old effect.
        if (counter_ == 0 && p.type != SlotType::Off)
            glide(p, e, false);
        while (h < hitCount && hits[h] <= i)
        {
            if (keyRatios && keyRatios[h] != keyRatio_)
            {
                keyRatio_ = keyRatios[h];
                if (p.type != SlotType::Off)
                    apply();
            }
            effect(type_).hit();
            ++h;
        }
        int end = std::min(n, i + (kStep - counter_));
        if (h < hitCount)
            end = std::min(end, hits[h]);
        segment(l + i, r + i, end - i, target);
        counter_ = (counter_ + end - i) % kStep;
        i = end;
    }
    if (target <= 0.0 && mix_ <= 0.0)
        live_ = false;
}

void Slot::segment(float* l, float* r, int n, double target)
{
    for (int k = 0; k < n; ++k)
    {
        dryL_[static_cast<size_t>(k)] = l[k];
        dryR_[static_cast<size_t>(k)] = r[k];
    }
    if (band_ == Band::Full)
    {
        selL_ = dryL_;
        selR_ = dryR_;
        restL_.fill(0.0f);
        restR_.fill(0.0f);
    }
    else
    {
        const bool lo = band_ == Band::Low || band_ == Band::LowMid;
        const bool mid = band_ == Band::Mid || band_ == Band::LowMid || band_ == Band::MidHigh;
        const bool hi = band_ == Band::High || band_ == Band::MidHigh;
        auto split = [&](Crossover3& x, float in, float& sel, float& rest) {
            double a, b, c;
            x.split(in, a, b, c);
            sel = static_cast<float>((lo ? a : 0.0) + (mid ? b : 0.0) + (hi ? c : 0.0));
            rest = static_cast<float>((lo ? 0.0 : a) + (mid ? 0.0 : b) + (hi ? 0.0 : c));
        };
        for (int k = 0; k < n; ++k)
        {
            const size_t s = static_cast<size_t>(k);
            split(xL_, dryL_[s], selL_[s], restL_[s]);
            split(xR_, dryR_[s], selR_[s], restR_[s]);
        }
    }
    wetL_ = selL_;
    wetR_ = selR_;
    Effect& fx = effect(type_);
    memoryDirty_ |= fx.memory(rate_) > 0;
    if (slotIsOversampled(type_) && osL_.factor() > 1)
    {
        const int m = n * osL_.factor();
        osL_.up(wetL_.data(), upL_.data(), n);
        osR_.up(wetR_.data(), upR_.data(), n);
        fx.process(upL_.data(), upR_.data(), m);
        osL_.down(upL_.data(), wetL_.data(), n);
        osR_.down(upR_.data(), wetR_.data(), n);
    }
    else
        fx.process(wetL_.data(), wetR_.data(), n);

    for (int k = 0; k < n; ++k)
    {
        const size_t s = static_cast<size_t>(k);
        mix_ += mixCoef_ * (target - mix_);
        if (std::fabs(mix_ - target) < 1e-6)
            mix_ = target;
        l[k] = static_cast<float>(restL_[s] + (selL_[s] + mix_ * (static_cast<double>(wetL_[s]) - selL_[s])));
        r[k] = static_cast<float>(restR_[s] + (selR_[s] + mix_ * (static_cast<double>(wetR_[s]) - selR_[s])));
    }
}

} // namespace substrike::dsp
