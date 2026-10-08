#include "TimeEffects.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace substrike::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLn1000 = 6.907755278982137;

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
double samples(double ms, double rate) { return std::max(ms * 0.001 * rate, 1.0); }

// After a time effect's last echo the lane still waits this long.
constexpr double kHoldMs = 10.0;

// A time from a Time letter and a Sync letter.
double timeMs(double ms, double sync, double tempo)
{
    const double beats = syncBeats(static_cast<int>(std::lround(sync)));
    return beats > 0.0 ? beats * 60000.0 / std::max(tempo, 1.0) : ms;
}

// A one-pole glide coefficient for delay times: a jump settles in about
// 50 ms, like a tape machine's speed, rather than in a click.
double timeGlide(double rate) { return 1.0 - std::exp(-1.0 / (0.05 * rate)); }

} // namespace

double syncBeats(int choice)
{
    // Free, 1/64, 1/32T, 1/32, 1/16T, 1/16, 1/16D, 1/8T, 1/8, 1/8D, 1/4T,
    // 1/4, 1/4D, 1/2, 1/2D, 1/1, in quarter notes.
    static const double beats[] = {0.0,        0.0625, 1.0 / 12.0, 0.125, 1.0 / 6.0, 0.25, 0.375, 1.0 / 3.0,
                                   0.5,        0.75,   2.0 / 3.0,  1.0,   1.5,       2.0,  3.0,   4.0};
    constexpr int n = static_cast<int>(sizeof(beats) / sizeof(beats[0]));
    return beats[std::clamp(choice, 0, n - 1)];
}

double DelayLine::read(double d) const
{
    d = std::clamp(d, 2.0, static_cast<double>(size - 3));
    const int i = static_cast<int>(d);
    const double f = d - i;
    const double xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
    const double c1 = 0.5 * (x1 - xm1);
    const double c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
    const double c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}

// ---------------------------------------------------------------------- Delay

void DelayFx::prepare(double sampleRate)
{
    rate_ = sampleRate;
    glide_ = timeGlide(sampleRate);
    hold_ = static_cast<int>(kHoldMs * 0.001 * sampleRate);
    lastColour_ = 1e9;
    reset();
}

size_t DelayFx::memory(double sampleRate) const { return 2 * static_cast<size_t>(lineSize(sampleRate)); }

void DelayFx::attach(float* mem)
{
    const int n = lineSize(rate_);
    line_[0].attach(mem, n);
    line_[1].attach(mem + n, n);
}

void DelayFx::reset()
{
    for (int c = 0; c < 2; ++c)
    {
        line_[c].reset();
        lp_[c].reset();
        hp_[c].reset();
    }
    snap_ = true;
}

void DelayFx::set(const double* v)
{
    const double ms = timeMs(v[0], v[1], tempo_);
    target_ = std::clamp(ms * 0.001 * rate_, 2.0, static_cast<double>(line_[0].size - 4));
    fb_ = std::clamp(v[2] / 100.0, 0.0, 0.99);
    if (v[3] != lastColour_)
    {
        lastColour_ = v[3];
        const double c = std::clamp(v[3] / 100.0, -1.0, 1.0);
        lowPass_ = c < 0.0;
        // Closed at -100 %: 400 Hz; open at 0: nothing. The highpass side
        // keeps 10 Hz at least, so no offset builds up in the loop.
        const double lp = 20000.0 * std::pow(400.0 / 20000.0, std::max(-c, 0.0));
        const double hp = 10.0 * std::pow(200.0, std::max(c, 0.0));
        for (int ch = 0; ch < 2; ++ch)
        {
            lp_[ch].setCutoff(static_cast<float>(lp), static_cast<float>(rate_));
            hp_[ch].setCutoff(static_cast<float>(hp), static_cast<float>(rate_));
        }
    }
    // 0 % is clean; 100 % starts clipping at -18 dB.
    drive_ = 1.0 + 7.0 * std::clamp(v[4] / 100.0, 0.0, 1.0);
    pingPong_ = std::clamp(v[5] / 100.0, 0.0, 1.0);
}

void DelayFx::process(float* l, float* r, int n)
{
    if (snap_)
    {
        cur_ = target_;
        snap_ = false;
    }
    const double pp = pingPong_;
    for (int i = 0; i < n; ++i)
    {
        cur_ += glide_ * (target_ - cur_);
        const double wl = line_[0].read(cur_), wr = line_[1].read(cur_);
        double loop[2] = {wl, wr};
        for (int c = 0; c < 2; ++c)
        {
            double x = loop[c];
            if (drive_ > 1.0)
                x = std::tanh(drive_ * x) / drive_;
            if (lowPass_)
                x = lp_[c].tick(static_cast<float>(x));
            loop[c] = hp_[c].tick(static_cast<float>(x));
        }
        // Ping-pong: the repeats cross sides, and the hit goes in on the left
        // only, so the first echo is on the left and the next on the right.
        const double bl = (1.0 - pp) * loop[0] + pp * loop[1];
        const double br = (1.0 - pp) * loop[1] + pp * loop[0];
        const double inL = (1.0 - pp) * l[i] + pp * 0.5 * (l[i] + r[i]);
        const double inR = (1.0 - pp) * r[i];
        line_[0].push(static_cast<float>(inL + fb_ * bl));
        line_[1].push(static_cast<float>(inR + fb_ * br));
        l[i] = static_cast<float>(l[i] + wl);
        r[i] = static_cast<float>(r[i] + wr);
    }
}

// ----------------------------------------------------------------------- Warp

void WarpFx::prepare(double sampleRate)
{
    rate_ = sampleRate;
    glide_ = timeGlide(sampleRate);
    for (OnePoleHp& d : dc_)
        d.setCutoff(10.0f, static_cast<float>(sampleRate));
    // Diffuser lengths in ms: mutually prime-ish, the longest under 20 ms.
    static const double kMs[kDiffusers] = {4.7, 7.3, 11.3, 17.9};
    for (int k = 0; k < kDiffusers; ++k)
        diffLen_[static_cast<size_t>(k)] = static_cast<int>(std::lround(kMs[k] * 0.001 * sampleRate));
    reset();
}

size_t WarpFx::memory(double sampleRate) const
{
    return 2 * static_cast<size_t>(lineSize(sampleRate)) +
           2 * kDiffusers * static_cast<size_t>(diffuserSize(sampleRate));
}

void WarpFx::attach(float* mem)
{
    const int n = lineSize(rate_), d = diffuserSize(rate_);
    line_[0].attach(mem, n);
    line_[1].attach(mem + n, n);
    float* p = mem + 2 * n;
    for (auto& ch : diff_)
        for (Allpass& a : ch)
        {
            a.line.attach(p, d);
            p += d;
        }
}

void WarpFx::reset()
{
    for (int c = 0; c < 2; ++c)
    {
        line_[c].reset();
        dc_[c].reset();
        for (Allpass& a : diff_[c])
            a.line.reset();
    }
    phase_ = 0.0;
    snap_ = true;
}

void WarpFx::set(const double* v)
{
    const double ms = timeMs(v[0], v[1], tempo_);
    mode_ = std::clamp(static_cast<int>(std::lround(v[4])), 0, 2);
    ratio_ = std::exp2(std::clamp(v[3], -12.0, 12.0) / 12.0);
    // Reverse reads up to (1 + ratio) windows back, so its window is shorter.
    const double room = static_cast<double>(line_[0].size - 8);
    const double most = mode_ == 1 ? room / (1.0 + ratio_) - 4.0 : room - 0.1 * rate_;
    target_ = std::clamp(ms * 0.001 * rate_, 4.0, most);
    fb_ = std::clamp(v[2] / 100.0, 0.0, 0.99);
    diffusion_ = 0.65 * std::clamp(v[5] / 100.0, 0.0, 1.0);
}

int WarpFx::silentHold() const
{
    const double span = mode_ == 1 ? (1.0 + ratio_) * time_ : time_ + window_;
    return static_cast<int>(span + kHoldMs * 0.001 * rate_);
}

double WarpFx::shifted(const DelayLine& line, double base) const
{
    // Two reads half a window apart, each faded in and out with sin^2, which
    // sums to one; the window drifts at (1 - ratio), which is the shift.
    const double a = phase_, b = phase_ + 0.5 - (phase_ >= 0.5 ? 1.0 : 0.0);
    const double ga = std::sin(kPi * a), gb = std::sin(kPi * b);
    return ga * ga * line.read(base + a * window_) + gb * gb * line.read(base + b * window_);
}

void WarpFx::process(float* l, float* r, int n)
{
    if (snap_)
    {
        time_ = target_;
        snap_ = false;
    }
    const bool pitched = std::fabs(ratio_ - 1.0) > 1e-4;
    for (int i = 0; i < n; ++i)
    {
        time_ += glide_ * (target_ - time_);
        double wet[2], fb[2];
        if (mode_ == 1)
        {
            // Reverse: within each window of Time the read runs backwards at
            // the pitch ratio, so the delay grows by (1 + ratio) per sample.
            const double span = (1.0 + ratio_) * time_;
            const double a = phase_, b = phase_ + 0.5 - (phase_ >= 0.5 ? 1.0 : 0.0);
            const double ga = std::sin(kPi * a), gb = std::sin(kPi * b);
            for (int c = 0; c < 2; ++c)
                wet[c] = fb[c] = ga * ga * line_[c].read(2.0 + a * span) + gb * gb * line_[c].read(2.0 + b * span);
            phase_ += 1.0 / time_;
        }
        else
        {
            window_ = std::min(0.06 * rate_, 0.5 * time_);
            for (int c = 0; c < 2; ++c)
                fb[c] = pitched ? shifted(line_[c], time_) : line_[c].read(time_);
            if (mode_ == 2)
            {
                // Four taps across Time, the first and third on the left, the
                // second and the last on the right; the last is the feedback.
                const double q = 0.25 * time_;
                wet[0] = 0.55 * line_[0].read(q) + 0.8 * line_[0].read(3.0 * q);
                wet[1] = 0.65 * line_[1].read(2.0 * q) + fb[1];
            }
            else
            {
                wet[0] = fb[0];
                wet[1] = fb[1];
            }
            if (pitched)
                phase_ += (1.0 - ratio_) / window_;
        }
        phase_ -= std::floor(phase_);
        for (int c = 0; c < 2; ++c)
        {
            double x = fb[c];
            if (diffusion_ > 0.0)
                for (int k = 0; k < kDiffusers; ++k)
                    x = diff_[c][k].tick(x, diffLen_[static_cast<size_t>(k)], diffusion_);
            x = dc_[c].tick(static_cast<float>(x));
            // A pitch shifter in a feedback loop can build up where its
            // windows overlap; a soft limit keeps a long tail bounded.
            x = std::tanh(x);
            line_[c].push(static_cast<float>((c == 0 ? l[i] : r[i]) + fb_ * x));
        }
        l[i] = static_cast<float>(l[i] + wet[0]);
        r[i] = static_cast<float>(r[i] + wet[1]);
    }
}

// --------------------------------------------------------------------- Reverb

namespace {
// Line lengths in ms at a size scale of one, and the input diffusers'.
constexpr double kLineMs[8] = {9.7, 12.3, 14.9, 17.9, 21.1, 24.7, 28.3, 33.1};
constexpr double kDiffMs[4] = {1.7, 2.9, 4.3, 6.1};
constexpr double kMaxScale = 2.0;
double sizeScale(double pct) { return 0.3 + 1.7 * std::clamp(pct / 100.0, 0.0, 1.0); }
int reverbLineSize(double ms, double rate) { return static_cast<int>(ms * kMaxScale * 0.001 * rate) + 4; }
} // namespace

void ReverbFx::prepare(double sampleRate)
{
    rate_ = sampleRate;
    hold_ = static_cast<int>(kHoldMs * 0.001 * sampleRate);
    last_.fill(1e9);
    reset();
}

size_t ReverbFx::memory(double sampleRate) const
{
    size_t n = 2 * static_cast<size_t>(kMaxPredelayMs * 0.001 * sampleRate + 8);
    for (double ms : kDiffMs)
        n += 2 * static_cast<size_t>(reverbLineSize(ms, sampleRate));
    for (double ms : kLineMs)
        n += static_cast<size_t>(reverbLineSize(ms, sampleRate));
    return n;
}

void ReverbFx::attach(float* mem)
{
    const int pre = static_cast<int>(kMaxPredelayMs * 0.001 * rate_ + 8);
    float* p = mem;
    for (DelayLine& d : pre_)
    {
        d.attach(p, pre);
        p += pre;
    }
    for (auto& ch : diff_)
        for (int k = 0; k < kDiffusers; ++k)
        {
            const int n = reverbLineSize(kDiffMs[k], rate_);
            ch[k].line.attach(p, n);
            p += n;
        }
    for (int k = 0; k < kLines; ++k)
    {
        const int n = reverbLineSize(kLineMs[k], rate_);
        line_[k].attach(p, n);
        p += n;
    }
}

void ReverbFx::reset()
{
    for (int c = 0; c < 2; ++c)
    {
        pre_[c].reset();
        lowCut_[c].reset();
        for (Allpass& a : diff_[c])
            a.line.reset();
    }
    for (int k = 0; k < kLines; ++k)
    {
        line_[k].reset();
        damp_[k].reset();
    }
}

void ReverbFx::set(const double* v)
{
    bool same = true;
    for (int i = 0; i < 6; ++i)
        same &= v[i] == last_[static_cast<size_t>(i)];
    if (same)
        return;
    for (int i = 0; i < 6; ++i)
        last_[static_cast<size_t>(i)] = v[i];

    const double scale = sizeScale(v[0]);
    const double rt60 = std::max(v[1], 10.0) * 0.001 * rate_;
    for (int k = 0; k < kLines; ++k)
    {
        const int len = std::max(2, static_cast<int>(std::lround(kLineMs[k] * scale * 0.001 * rate_)));
        len_[static_cast<size_t>(k)] = len;
        gain_[static_cast<size_t>(k)] = std::pow(10.0, -3.0 * len / rt60);
        damp_[k].setCutoff(static_cast<float>(v[2]), static_cast<float>(rate_));
    }
    for (int k = 0; k < kDiffusers; ++k)
        diffLen_[static_cast<size_t>(k)] = std::max(1, static_cast<int>(std::lround(kDiffMs[k] * scale * 0.001 * rate_)));
    predelay_ = std::clamp(v[3] * 0.001 * rate_, 0.0, kMaxPredelayMs * 0.001 * rate_);
    lowCutHz_ = v[4];
    lowCutOn_ = v[4] >= 20.05; // the bottom of the range shows as Off
    if (lowCutOn_)
        for (Svf& f : lowCut_)
            f.setup(v[4], 0.7071067811865476, rate_);
    width_ = std::clamp(v[5] / 100.0, 0.0, 1.0);
}

void ReverbFx::process(float* l, float* r, int n)
{
    // The input diffusers' coefficient, and the output scale that keeps a
    // hit through the reverb at about the level it went in.
    constexpr double kDiffG = 0.6;
    constexpr double kOut = 0.6;
    const int pre = static_cast<int>(std::lround(predelay_));
    for (int i = 0; i < n; ++i)
    {
        double in[2] = {l[i], r[i]};
        for (int c = 0; c < 2; ++c)
        {
            double x = in[c];
            if (lowCutOn_)
                x = lowCut_[c].highPass(x);
            if (pre > 0)
            {
                const double d = pre_[c].at(pre);
                pre_[c].push(static_cast<float>(x));
                x = d;
            }
            for (int k = 0; k < kDiffusers; ++k)
                x = diff_[c][k].tick(x, diffLen_[static_cast<size_t>(k)], kDiffG);
            in[c] = x;
        }

        double y[kLines];
        for (int k = 0; k < kLines; ++k)
            y[k] = damp_[k].tick(line_[k].at(len_[static_cast<size_t>(k)])) * gain_[static_cast<size_t>(k)];
        // A fast Walsh-Hadamard transform: a lossless mix of all eight lines.
        double h[kLines];
        for (int k = 0; k < kLines; ++k)
            h[k] = y[k];
        for (int span = 1; span < kLines; span *= 2)
            for (int a = 0; a < kLines; a += 2 * span)
                for (int b = a; b < a + span; ++b)
                {
                    const double u = h[b], v = h[b + span];
                    h[b] = u + v;
                    h[b + span] = u - v;
                }
        constexpr double norm = 0.35355339059327373; // 1 / sqrt(8)
        for (int k = 0; k < kLines; ++k)
            line_[k].push(static_cast<float>(h[k] * norm + in[k & 1]));

        // Two decorrelated sums of the lines, then the width.
        const double outL = (y[0] - y[1] + y[2] - y[3] + y[4] - y[5] + y[6] - y[7]) * norm * kOut;
        const double outR = (y[0] + y[1] - y[2] - y[3] + y[4] + y[5] - y[6] - y[7]) * norm * kOut;
        const double mid = 0.5 * (outL + outR), side = 0.5 * (outL - outR) * width_;
        l[i] = static_cast<float>(mid + side);
        r[i] = static_cast<float>(mid - side);
    }
}

// ---------------------------------------------------------------------- Smear

namespace {
// Stage lengths as fractions of Time, longest first, and a second set for the
// right channel that Width moves towards.
constexpr double kSmearL[16] = {1.0,   0.873, 0.761, 0.659, 0.577, 0.503, 0.437, 0.383,
                                0.331, 0.289, 0.251, 0.219, 0.191, 0.167, 0.143, 0.127};
constexpr double kSmearR[16] = {0.941, 0.811, 0.797, 0.613, 0.601, 0.467, 0.449, 0.359,
                                0.347, 0.271, 0.263, 0.207, 0.197, 0.157, 0.149, 0.119};
} // namespace

void SmearFx::prepare(double sampleRate)
{
    rate_ = sampleRate;
    reset();
}

size_t SmearFx::memory(double sampleRate) const
{
    return 2 * kStages * static_cast<size_t>(stageSize(sampleRate));
}

void SmearFx::attach(float* mem)
{
    const int n = stageSize(rate_);
    for (auto& ch : ap_)
        for (Allpass& a : ch)
        {
            a.line.attach(mem, n);
            mem += n;
        }
}

void SmearFx::reset()
{
    for (auto& ch : ap_)
        for (Allpass& a : ch)
            a.line.reset();
}

void SmearFx::set(const double* v)
{
    static const int kCounts[] = {4, 8, 12, 16};
    stages_ = kCounts[std::clamp(static_cast<int>(std::lround(v[2])), 0, 3)];
    // Time is spread over the stages in use, so more stages smear more
    // finely rather than for longer.
    double sum = 0.0;
    for (int k = 0; k < stages_; ++k)
        sum += kSmearL[k];
    const double base = std::clamp(v[0], 0.1, kMaxMs) * 0.001 * rate_ / sum;
    const double width = std::clamp(v[3] / 100.0, 0.0, 1.0);
    for (int k = 0; k < kStages; ++k)
    {
        const double right = kSmearL[k] + width * (kSmearR[k] - kSmearL[k]);
        len_[0][static_cast<size_t>(k)] = std::max(1, static_cast<int>(std::lround(base * kSmearL[k])));
        len_[1][static_cast<size_t>(k)] = std::max(1, static_cast<int>(std::lround(base * right)));
    }
    // Past about 0.7 an allpass rings on long enough to be heard as a tail.
    g_ = 0.7 * std::clamp(v[1] / 100.0, 0.0, 1.0);
}

void SmearFx::process(float* l, float* r, int n)
{
    float* ch[2] = {l, r};
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
        {
            double x = ch[c][i];
            for (int k = 0; k < stages_; ++k)
                x = ap_[c][k].tick(x, len_[static_cast<size_t>(c)][static_cast<size_t>(k)], g_);
            ch[c][i] = static_cast<float>(x);
        }
}

// ----------------------------------------------------------------------- Ring

namespace {
// a^2 of the two chains (O. Niemitalo, "Hilbert transform: polyphase IIR").
constexpr double kHilbertA[4] = {0.6923878 * 0.6923878, 0.9360654322959 * 0.9360654322959,
                                 0.9882295226860 * 0.9882295226860, 0.9987488452737 * 0.9987488452737};
constexpr double kHilbertB[4] = {0.4021921162426 * 0.4021921162426, 0.8561710882420 * 0.8561710882420,
                                 0.9722909545651 * 0.9722909545651, 0.9952884791278 * 0.9952884791278};

// One chain: y = a^2 (x + y[n-2]) - x[n-2] per stage. s[k] = {x1, x2, y1, y2}.
double chain(const double* a2, std::array<std::array<double, 4>, 4>& s, double x)
{
    for (int k = 0; k < 4; ++k)
    {
        auto& st = s[static_cast<size_t>(k)];
        const double y = a2[k] * (x + st[3]) - st[1];
        st[1] = st[0];
        st[0] = x;
        st[3] = st[2];
        st[2] = y;
        x = y;
    }
    return x;
}
} // namespace

void RingFx::prepare(double sampleRate)
{
    rate_ = sampleRate;
    reset();
}

void RingFx::reset()
{
    for (auto& c : ha_)
        for (auto& s : c)
            s.fill(0.0);
    for (auto& c : hb_)
        for (auto& s : c)
            s.fill(0.0);
    delayed_.fill(0.0);
    phase_ = 0.0;
    env_ = 0.0;
}

void RingFx::set(const double* v)
{
    mode_ = std::clamp(static_cast<int>(std::lround(v[0])), 0, 2);
    freq_ = v[1];
    envOct_ = v[2];
    envFall_ = std::exp(-kLn1000 / samples(v[3], rate_));
}

void RingFx::hit()
{
    phase_ = 0.0;
    env_ = 1.0;
}

void RingFx::process(float* l, float* r, int n)
{
    float* ch[2] = {l, r};
    for (int i = 0; i < n; ++i)
    {
        double f = freq_;
        if (envOct_ != 0.0 && env_ > 1e-5)
        {
            f *= std::exp2(envOct_ * env_);
            env_ *= envFall_;
        }
        const double c = std::cos(2.0 * kPi * phase_), s = std::sin(2.0 * kPi * phase_);
        phase_ += std::min(f, 0.45 * rate_) / rate_;
        phase_ -= std::floor(phase_);
        for (int k = 0; k < 2; ++k)
        {
            const double x = ch[k][i];
            if (mode_ == 0)
            {
                ch[k][i] = static_cast<float>(x * c);
                continue;
            }
            // The first chain, one sample late, and the second, which runs
            // 90 degrees ahead of it: the analytic signal is re - j im.
            const double re = delayed_[static_cast<size_t>(k)];
            delayed_[static_cast<size_t>(k)] = chain(kHilbertA, ha_[static_cast<size_t>(k)], x);
            const double im = chain(kHilbertB, hb_[static_cast<size_t>(k)], x);
            ch[k][i] = static_cast<float>(mode_ == 1 ? re * c + im * s : re * c - im * s);
        }
    }
}

// --------------------------------------------------------------------- Stereo

void StereoFx::prepare(double sampleRate)
{
    rate_ = sampleRate;
    reset();
}

size_t StereoFx::memory(double sampleRate) const { return 2 * static_cast<size_t>(lineSize(sampleRate)); }

void StereoFx::attach(float* mem)
{
    const int n = lineSize(rate_);
    line_[0].attach(mem, n);
    line_[1].attach(mem + n, n);
}

void StereoFx::reset()
{
    line_[0].reset();
    line_[1].reset();
}

void StereoFx::set(const double* v)
{
    width_ = std::clamp(v[0] / 100.0, 0.0, 2.0);
    haas_ = std::clamp(v[1], -kMaxHaasMs, kMaxHaasMs) * 0.001 * rate_;
}

void StereoFx::process(float* l, float* r, int n)
{
    const double dl = std::max(-haas_, 0.0), dr = std::max(haas_, 0.0);
    for (int i = 0; i < n; ++i)
    {
        const double mid = 0.5 * (l[i] + r[i]), side = 0.5 * (l[i] - r[i]) * width_;
        const double a = mid + side, b = mid - side;
        // The delayed side; at 0 ms the line is passed by.
        const double ya = dl >= 1.0 ? line_[0].read(std::max(dl, 2.0)) : a;
        const double yb = dr >= 1.0 ? line_[1].read(std::max(dr, 2.0)) : b;
        line_[0].push(static_cast<float>(a));
        line_[1].push(static_cast<float>(b));
        l[i] = static_cast<float>(ya);
        r[i] = static_cast<float>(yb);
    }
}

// -------------------------------------------------------------------- Utility

void UtilityFx::set(const double* v)
{
    gain_ = v[0] <= -47.95 ? 0.0 : dbToGain(v[0]);
    if (std::lround(v[1]) == 1)
        gain_ = -gain_;
    channels_ = std::clamp(static_cast<int>(std::lround(v[2])), 0, 4);
}

void UtilityFx::process(float* l, float* r, int n)
{
    for (int i = 0; i < n; ++i)
    {
        double a = l[i], b = r[i];
        switch (channels_)
        {
        case 1: a = b = 0.5 * (a + b); break;
        case 2: std::swap(a, b); break;
        case 3: b = a; break;
        case 4: a = b; break;
        default: break;
        }
        l[i] = static_cast<float>(a * gain_);
        r[i] = static_cast<float>(b * gain_);
    }
}

} // namespace substrike::dsp
