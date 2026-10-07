#include "Sources.h"

#include <algorithm>
#include <cmath>

namespace substrike::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
// ln(1000): an exponential decay over T reaches -60 dB at T.
constexpr double kLn1000 = 6.907755278982137;
// The longest click: Click Decay's upper end.
constexpr double kMaxClickMs = 100.0;

double frac(double x) { return x - std::floor(x); }

// Band-limited step and ramp residuals (Välimäki et al.), for a discontinuity
// at phase 0 of a phase t in [0, 1) advancing by dt per sample.
double polyBlep(double t, double dt)
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt)
    {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

double polyBlamp(double t, double dt)
{
    if (t < dt)
    {
        t = t / dt - 1.0;
        return -t * t * t / 3.0;
    }
    if (t > 1.0 - dt)
    {
        t = (t - 1.0) / dt + 1.0;
        return t * t * t / 3.0;
    }
    return 0.0;
}

// The analogue waveforms, all phase-aligned with a sine: zero and rising at
// phase 0 (the square jumps there instead).
double triangle(double t, double dt)
{
    double y = t < 0.25 ? 4.0 * t : t < 0.75 ? 2.0 - 4.0 * t : 4.0 * t - 4.0;
    // The slope turns by -8 at a quarter turn and by +8 at three quarters.
    y += -8.0 * dt * polyBlamp(frac(t - 0.25), dt);
    y += 8.0 * dt * polyBlamp(frac(t - 0.75), dt);
    return y;
}

double saw(double t, double dt)
{
    const double u = frac(t + 0.5);
    return 2.0 * u - 1.0 - polyBlep(u, dt);
}

double square(double t, double dt)
{
    return (t < 0.5 ? 1.0 : -1.0) + polyBlep(t, dt) - polyBlep(frac(t + 0.5), dt);
}

// The attack-hold-decay shape every source with an amplitude envelope uses.
// Returns a negative value once the decay is over.
double ahd(double ms, double attack, double holdEnd, double decay, double curve)
{
    if (ms < attack)
        return ms / attack;
    if (ms < holdEnd)
        return 1.0;
    const double x = (ms - holdEnd) / decay;
    if (x >= 1.0)
        return -1.0;
    return 1.0 - segmentShape(x, curve);
}

} // namespace

double PitchTrack::endFreq(double semitones) const
{
    return end * std::exp2(semitones * keyTrack / 12.0) * ratio;
}

// ----------------------------------------------------------------------- Voice

void Voice::begin()
{
    active_ = true;
    fade_ = 1.0;
    fadeStep_ = 0.0;
}

void Voice::fadeOut(int samples)
{
    if (!active_ || fading())
        return;
    fadeStep_ = 1.0 / std::max(samples, 1);
}

// ------------------------------------------------------------------- BodyVoice

void BodyVoice::start(const Hit& hit, double phase)
{
    begin();
    hit_ = hit;
    rng_.seed_(hit.seed);
    t_ = 0.0;
    phase_ = 0.0;
    phase0_ = phase - std::floor(phase);
    modPhase_ = 0.0;
    partialPhase_.fill(0.0);
    y1_ = y2_ = 0.0;
    drift_ = rng_.bipolar();
    driftTarget_ = drift_;
}

void BodyVoice::render(float* l, float* r, int n, double sampleRate, const BodyParams& p, const PitchTrack& track,
                       const Curve& amp)
{
    if (!active_)
        return;
    const double msPerSample = 1000.0 / sampleRate;
    const double invRate = 1.0 / sampleRate;
    const double fEnd = track.endFreq(hit_.semitones) * hit_.pitchRatio;
    const double logRatio = std::log(track.start / track.end);
    const double fMax = 0.45 * sampleRate;
    const double sweep = std::max(track.sweepMs, 0.01);
    const double attack = std::max(p.attackMs, 0.0);
    const double holdEnd = attack + std::max(p.holdMs, 0.0);
    const double decay = std::max(p.decayMs * hit_.timeScale, 0.1);
    const double level = hit_.level;

    // Phase modulation, in turns: the FM index is up to 8 radians and the
    // feedback up to 1.5, past which the feedback oscillator turns to noise.
    const double fmIndex = 8.0 * std::clamp(p.fmAmount, 0.0, 1.0) / kTwoPi;
    const double fmFall = -kLn1000 / std::max(p.fmDecayMs, 0.1);
    const double fb = 1.5 * std::clamp(p.feedback, 0.0, 1.0) / kTwoPi;
    const double shapeGain = 15.0 * p.shape * p.shape;
    const double shapeNorm = shapeGain > 0.0 ? 1.0 / std::tanh(shapeGain) : 1.0;
    // Up to 40 cents of slow wander, a new target every 20 ms, smoothed over
    // about 50 ms.
    const double driftDepth = std::clamp(p.drift, 0.0, 1.0) * 40.0 / 1200.0;
    const int driftStep = std::max(1, static_cast<int>(0.02 * sampleRate));
    const double driftCoef = 1.0 - std::exp(-1.0 / (0.05 * sampleRate));

    // Additive: 8 partials, from 1/k^2 (tilt -1) over a saw's 1/k to flat
    // (tilt +1), stretched apart like a stiff string and normalised by the sum
    // of their levels, so the waveform never leaves full scale.
    std::array<double, kPartials> ratio{}, gain{};
    double norm = 1.0;
    const bool additive = p.wave == Wave::Additive;
    if (additive)
    {
        const double b = 0.02 * std::clamp(p.stretch, 0.0, 1.0);
        const double exponent = -1.0 + std::clamp(p.tilt, -1.0, 1.0);
        double sum = 0.0;
        for (int k = 0; k < kPartials; ++k)
        {
            const double h = k + 1.0;
            ratio[k] = h * std::sqrt(1.0 + b * (h * h - 1.0));
            gain[k] = std::pow(h, exponent) * ((k & 1) ? std::clamp(p.even, 0.0, 1.0) : 1.0);
            sum += gain[k];
        }
        norm = 1.0 / sum;
    }

    for (int i = 0; i < n; ++i)
    {
        const double ms = t_ * msPerSample;
        const double a = ms < attack ? ms / attack : ms < holdEnd ? 1.0 : 0.0;
        double env = a;
        if (ms >= holdEnd)
        {
            const double x = (ms - holdEnd) / decay;
            if (x >= 1.0)
            {
                active_ = false;
                return;
            }
            env = amp.eval(x, p.decayCurve);
        }

        double f = fEnd * std::exp(logRatio * track.curve->eval(ms / sweep, track.sweepCurve));
        if (static_cast<long long>(t_) % driftStep == 0)
            driftTarget_ = rng_.bipolar();
        drift_ += driftCoef * (driftTarget_ - drift_);
        if (driftDepth > 0.0)
            f *= std::exp2(driftDepth * drift_);
        f = std::min(f, fMax);
        const double dt = f * invRate;

        double pm = 0.0;
        if (fmIndex > 0.0)
            pm += fmIndex * std::exp(fmFall * ms) * std::sin(kTwoPi * modPhase_);
        if (fb > 0.0)
            pm += fb * 0.5 * (y1_ + y2_);

        double y;
        const double ph = phase_ + phase0_ + pm;
        switch (p.wave)
        {
        case Wave::Triangle: y = triangle(frac(ph), dt); break;
        case Wave::Saw: y = saw(frac(ph), dt); break;
        case Wave::Square: y = square(frac(ph), dt); break;
        case Wave::Additive:
        {
            y = 0.0;
            for (int k = 0; k < kPartials; ++k)
                if (ratio[k] * f < fMax)
                    y += gain[k] * std::sin(kTwoPi * (partialPhase_[k] + ratio[k] * (phase0_ + pm)));
            y *= norm;
            break;
        }
        case Wave::Sine:
        default: y = std::sin(kTwoPi * ph); break;
        }
        y2_ = y1_;
        y1_ = y;
        if (shapeGain > 0.0)
            y = std::tanh(shapeGain * y) * shapeNorm;

        l[i] += static_cast<float>(y * env * level * fade_);
        r[i] += static_cast<float>(y * env * level * fade_);

        phase_ += dt;
        phase_ -= std::floor(phase_);
        modPhase_ += p.fmRatio * dt;
        modPhase_ -= std::floor(modPhase_);
        if (additive)
            for (int k = 0; k < kPartials; ++k)
                partialPhase_[k] = frac(partialPhase_[k] + ratio[k] * dt);
        t_ += 1.0;
        if (!stepFade())
            return;
    }
}

// ------------------------------------------------------------------ ClickVoice

void ClickVoice::prepare(double sampleRate)
{
    buffer_.assign(static_cast<size_t>(std::ceil(kMaxClickMs * 0.001 * sampleRate)) + 1, 0.0f);
    active_ = false;
}

void ClickVoice::start(const Hit& hit, const ClickParams& p, double pitch, double cutoff, double sampleRate)
{
    if (buffer_.empty())
        return;
    begin();
    const double decayMs = std::clamp(p.decayMs * hit.timeScale, 0.05, kMaxClickMs);
    length_ = std::clamp(static_cast<int>(std::ceil(decayMs * 0.001 * sampleRate)), 1,
                         static_cast<int>(buffer_.size()));
    pos_ = 0;

    Rng rng(hit.seed);
    Svf svf;
    svf.setup(cutoff, resonanceToQ(p.reso), sampleRate);
    const double fall = -kLn1000 / length_;
    const double fMax = 0.45 * sampleRate;
    // The zap falls through Sweep octaves onto Pitch with a time constant of
    // a sixth of the decay: a sub-millisecond chirp at the short settings.
    const double zapTau = std::max(1.0, length_ / 6.0);
    double phase = 0.0;
    float peak = 0.0f;
    for (int i = 0; i < length_; ++i)
    {
        double x = 0.0;
        switch (p.type)
        {
        case ClickType::Impulse: x = i == 0 ? 1.0 : 0.0; break;
        case ClickType::Noise: x = rng.bipolar(); break;
        case ClickType::Blip:
        case ClickType::Zap:
        {
            double f = pitch;
            if (p.type == ClickType::Zap)
                f *= std::exp2(p.sweepOct * std::exp(-i / zapTau));
            x = std::sin(kTwoPi * phase);
            phase = frac(phase + std::min(f, fMax) / sampleRate);
            break;
        }
        }
        const float y = static_cast<float>(svf.process(x, p.filter) * std::exp(fall * i));
        buffer_[static_cast<size_t>(i)] = y;
        peak = std::max(peak, std::fabs(y));
    }
    const float scale = peak > 1e-9f ? static_cast<float>(hit.level) / peak : 0.0f;
    for (int i = 0; i < length_; ++i)
        buffer_[static_cast<size_t>(i)] *= scale;
}

void ClickVoice::render(float* l, float* r, int n)
{
    if (!active_)
        return;
    for (int i = 0; i < n; ++i)
    {
        if (pos_ >= length_)
        {
            active_ = false;
            return;
        }
        const float y = static_cast<float>(buffer_[static_cast<size_t>(pos_++)] * fade_);
        l[i] += y;
        r[i] += y;
        if (!stepFade())
            return;
    }
}

// ------------------------------------------------------------------ NoiseVoice

double NoiseVoice::Colour::next(NoiseColor c, double white, double crackleChance, Rng& rng)
{
    switch (c)
    {
    case NoiseColor::Pink:
    {
        // P. Kellet's economy filter, -3 dB per octave within 0.05 dB above
        // 9 Hz at 44.1 kHz; scaled to the RMS of the white noise.
        b0 = 0.99886 * b0 + white * 0.0555179;
        b1 = 0.99332 * b1 + white * 0.0750759;
        b2 = 0.96900 * b2 + white * 0.1538520;
        b3 = 0.86650 * b3 + white * 0.3104856;
        b4 = 0.55000 * b4 + white * 0.5329522;
        b5 = -0.7616 * b5 - white * 0.0168980;
        const double pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362;
        b6 = white * 0.115926;
        return pink * 0.336;
    }
    case NoiseColor::Brown:
        brown = 0.995 * brown + 0.1 * white;
        return brown * 1.05;
    case NoiseColor::Crackle:
    {
        const double u = rng.unit();
        return u < crackleChance ? white : 0.0;
    }
    case NoiseColor::White:
    default: return white;
    }
}

void NoiseVoice::start(const Hit& hit)
{
    begin();
    hit_ = hit;
    rngA_.seed_(hit.seed);
    rngB_.seed_(hit.seed ^ 0xA5A5A5A55A5A5A5Aull);
    colA_ = Colour{};
    colB_ = Colour{};
    svfL_.reset();
    svfR_.reset();
    t_ = 0.0;
}

void NoiseVoice::render(float* l, float* r, int n, double sampleRate, const NoiseParams& p, double freqRatio)
{
    if (!active_)
        return;
    const double msPerSample = 1000.0 / sampleRate;
    const double attack = std::max(p.attackMs, 0.0);
    const double holdEnd = attack + std::max(p.holdMs, 0.0);
    const double decay = std::max(p.decayMs * hit_.timeScale, 0.1);
    const double crackle = 20.0 * std::pow(1000.0, std::clamp(p.density, 0.0, 1.0)) / sampleRate;
    const double w = std::clamp(p.width, 0.0, 1.0);
    const double widthNorm = 1.0 / std::sqrt(1.0 + w * w);
    const double q = resonanceToQ(p.reso);
    // A resonant filter passes noise power in proportion to Q; this keeps the
    // level roughly where it was as the resonance goes up.
    const double qGain = std::min(1.0, std::sqrt(0.7071 / q));
    const double cutoff = p.cutoff * freqRatio;
    const double envFall = -kLn1000 / std::max(p.envDecayMs, 0.1);
    const bool filtered = p.filter != FilterMode::Off;
    const bool envMoves = filtered && p.filterEnvOct != 0.0;
    if (filtered && !envMoves)
    {
        svfL_.setup(cutoff, q, sampleRate);
        svfR_.setup(cutoff, q, sampleRate);
    }

    for (int i = 0; i < n; ++i)
    {
        const double ms = t_ * msPerSample;
        const double env = ahd(ms, attack, holdEnd, decay, p.curve);
        if (env < 0.0)
        {
            active_ = false;
            return;
        }
        const double a = colA_.next(p.color, rngA_.bipolar(), crackle, rngA_);
        const double b = colB_.next(p.color, rngB_.bipolar(), crackle, rngB_);
        double left = (a + w * b) * widthNorm;
        double right = (a - w * b) * widthNorm;
        if (filtered)
        {
            if (envMoves)
            {
                const double c = cutoff * std::exp2(p.filterEnvOct * std::exp(envFall * ms));
                svfL_.setup(c, q, sampleRate);
                svfR_.setup(c, q, sampleRate);
            }
            left = svfL_.process(left, p.filter) * qGain;
            right = svfR_.process(right, p.filter) * qGain;
        }
        const double g = env * hit_.level * fade_;
        l[i] += static_cast<float>(left * g);
        r[i] += static_cast<float>(right * g);
        t_ += 1.0;
        if (!stepFade())
            return;
    }
}

// -------------------------------------------------------------- ResonatorVoice

namespace {

// Mode frequencies relative to the lowest. Membrane: the ideal circular
// membrane (zeros of the Bessel functions). Bar: a free-free bar.
constexpr double kMembrane[8] = {1.0, 1.594, 2.136, 2.296, 2.653, 2.918, 3.156, 3.501};
constexpr double kBar[8] = {1.0, 2.756, 5.404, 8.933, 13.344, 18.638, 24.814, 31.873};

double modeRatio(ResonatorModel m, int k)
{
    switch (m)
    {
    case ResonatorModel::Harmonic: return k + 1.0;
    case ResonatorModel::Odd: return 2.0 * k + 1.0;
    case ResonatorModel::Bar: return kBar[k];
    case ResonatorModel::Membrane:
    default: return kMembrane[k];
    }
}

// The voice checks its remaining energy this often once the exciter is over.
constexpr int kSilenceCheck = 256;
// A pitch drop updates the mode rotations this often.
constexpr int kDropStep = 16;

} // namespace

void ResonatorVoice::start(const Hit& hit, const ResonatorParams& p, double tune, double sampleRate)
{
    begin();
    hit_ = hit;
    rng_.seed_(hit.seed);
    sampleRate_ = sampleRate;
    count_ = std::clamp(p.modes, 2, kMaxModes);
    const double t60 = std::max(p.decayMs * hit.timeScale, 1.0) * 0.001;
    const double damping = std::clamp(p.damping, 0.0, 1.0);
    const double dark = 2.0 * (1.0 - std::clamp(p.brightness, 0.0, 1.0));
    double sum = 0.0;
    for (int k = 0; k < count_; ++k)
    {
        const double ratio = modeRatio(p.model, k);
        omega_[k] = kTwoPi * tune * ratio / sampleRate;
        radius_[k] = std::exp(-kLn1000 / (t60 * std::pow(ratio, -2.0 * damping) * sampleRate));
        amp_[k] = std::pow(ratio, -dark);
        sum += amp_[k];
        zRe_[k] = zIm_[k] = 0.0;
    }
    // Struck by an impulse, the modes cannot add up past full scale.
    norm_ = 1.0 / sum;

    exciter_ = p.exciter;
    const double hard = std::clamp(p.hardness, 0.0, 1.0);
    switch (exciter_)
    {
    case Exciter::Impulse: exciteLength_ = 1; break;
    case Exciter::Mallet:
        // A raised-cosine pulse from 8 ms (soft) to 0.25 ms (hard).
        exciteLength_ = std::max(1, static_cast<int>(8.0 * std::pow(1.0 / 32.0, hard) * 0.001 * sampleRate));
        break;
    case Exciter::Noise:
    {
        // A decaying burst, 30 ms to 3 ms, low-passed from 500 Hz to 16 kHz.
        exciteLength_ = std::max(1, static_cast<int>(30.0 * std::pow(0.1, hard) * 0.001 * sampleRate));
        const double fc = std::min(500.0 * std::pow(32.0, hard), 0.45 * sampleRate);
        noiseLpCoef_ = 1.0 - std::exp(-kTwoPi * fc / sampleRate);
        noiseLp_ = 0.0;
        // Unit expected energy over the burst, so a mode responds about as
        // strongly as to the impulse.
        noiseScale_ = std::sqrt(30.0 / exciteLength_);
        break;
    }
    }
    dropOct_ = std::max(p.dropSt, 0.0) / 12.0;
    // 99 % of the drop is over at Drop Time.
    dropTau_ = std::max(p.dropMs, 0.1) * 0.001 * sampleRate / 4.6;
    t_ = 0;
    updateRotations();
}

void ResonatorVoice::updateRotations()
{
    const double m = dropOct_ > 0.0 ? std::exp2(dropOct_ * std::exp(-t_ / dropTau_)) : 1.0;
    for (int k = 0; k < count_; ++k)
    {
        const double w = omega_[k] * m;
        gain_[k] = w < 0.45 * kTwoPi ? 1.0 : 0.0;
        rotRe_[k] = radius_[k] * std::cos(w);
        rotIm_[k] = radius_[k] * std::sin(w);
    }
}

void ResonatorVoice::render(float* l, float* r, int n)
{
    if (!active_)
        return;
    const double g = norm_ * hit_.level;
    for (int i = 0; i < n; ++i)
    {
        if (dropOct_ > 0.0 && t_ % kDropStep == 0 && t_ < 10.0 * dropTau_)
            updateRotations();

        double x = 0.0;
        if (t_ < exciteLength_)
        {
            switch (exciter_)
            {
            case Exciter::Impulse: x = 1.0; break;
            case Exciter::Mallet:
                x = (1.0 - std::cos(kTwoPi * (t_ + 0.5) / exciteLength_)) / exciteLength_;
                break;
            case Exciter::Noise:
                noiseLp_ += noiseLpCoef_ * (rng_.bipolar() * std::exp(-5.0 * t_ / exciteLength_) - noiseLp_);
                x = noiseLp_ * noiseScale_;
                break;
            }
        }
        else if ((t_ - exciteLength_) % kSilenceCheck == 0)
        {
            double e = 0.0;
            for (int k = 0; k < count_; ++k)
                e += zRe_[k] * zRe_[k] + zIm_[k] * zIm_[k];
            // -120 dB
            if (e < 1e-12)
            {
                active_ = false;
                return;
            }
        }

        double y = 0.0;
        for (int k = 0; k < count_; ++k)
        {
            const double re = rotRe_[k] * zRe_[k] - rotIm_[k] * zIm_[k] + amp_[k] * x;
            const double im = rotIm_[k] * zRe_[k] + rotRe_[k] * zIm_[k];
            zRe_[k] = re;
            zIm_[k] = im;
            y += im * gain_[k];
        }
        const float out = static_cast<float>(y * g * fade_);
        l[i] += out;
        r[i] += out;
        ++t_;
        if (!stepFade())
            return;
    }
}

} // namespace substrike::dsp
