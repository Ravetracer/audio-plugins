#pragma once

#include <array>

#include "Math.h"
#include "Svf.h"

namespace aurum::dsp {

enum class PostShape : int { Bell = 0, LowShelf = 1, HighShelf = 2, LowCut = 3, HighCut = 4 };
enum class Placement : int { Stereo = 0, Left = 1, Right = 2, Mid = 3, Side = 4 };

constexpr int kNumPostBands = 6;
constexpr int kNumSlopes = 9;
// Cut slopes in dB/oct, indexed by the slope parameter.
inline constexpr int kSlopeDb[kNumSlopes] = {6, 12, 18, 24, 30, 36, 48, 72, 96};

struct PostBand
{
    bool used = false;
    bool enabled = true;
    PostShape shape = PostShape::Bell;
    double freq = 1000.0;
    double gainDb = 0.0;
    double q = 1.0;
    int slope = 3; // index into kSlopeDb
    Placement placement = Placement::Stereo;
};

// Six-band equaliser for the wet signal with per-band stereo placement and
// energy based auto gain compensation.
class PostEq
{
public:
    void prepare(double sampleRate);
    void reset();
    void setBands(const std::array<PostBand, kNumPostBands>& bands);
    void process(float* L, float* R, int n);

    // Power response of one band (both analog-exact and bilinear-warped).
    static double bandPow(const PostBand& b, double f, double fs);
    // Combined power response seen by channel c (0 = left, 1 = right) assuming
    // decorrelated left/right content.
    static double channelPow(const std::array<PostBand, kNumPostBands>& bands, double f, double fs, int channel);
    // Gain (dB) that keeps the average energy constant over the audio band.
    static double compensationDb(const std::array<PostBand, kNumPostBands>& bands, double fs);

    float compensationGain() const { return compGain_; }

private:
    static constexpr int kMaxStages = 8;

    struct BandState
    {
        PostBand target;
        PostBand current; // smoothed
        bool active = false;
        int numStages = 0;
        bool onePole = false;
        bool onePoleHigh = false;
        std::array<SvfCoeffs, kMaxStages> coeffs{};
        OnePoleCoeffs op{};
        std::array<std::array<SvfState, kMaxStages>, 2> svf{};
        std::array<OnePoleState, 2> ops{};
        bool dirty = true;
    };

    void design(BandState& b);
    void processChannel(BandState& b, int ch, float* x, int n);

    double fs_ = 48000.0;
    std::array<BandState, kNumPostBands> bands_{};
    float compGain_ = 1.0f;
    float compTarget_ = 1.0f;
    float smooth_ = 0.0f;
};

} // namespace aurum::dsp
