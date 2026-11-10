#include "SidChip.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "residfp/residfp.h"
#include "sidlite/SID.h"

namespace sidmidi::emu {

namespace {

// ---------------------------------------------------------------- reSIDfp

class ReSidFpChip final : public SidChip
{
public:
    SidEngine engine() const override { return SidEngine::ReSidFp; }

    void tune(const SidChipConfig& cc, const SidChipConfig& was, bool valid) override
    {
        // A new chip model rebuilds tables, a filter curve does not, and a
        // slider drag must stay cheap.
        const bool is8580 = sidVariantIs8580(cc.variant);
        const bool modelChanged = !valid || sidVariantIs8580(was.variant) != is8580;
        if (modelChanged)
            sid_.setChipModel(is8580 ? reSIDfp::CSG8580 : reSIDfp::MOS6581);
        if (modelChanged || was.tuning.combined != cc.tuning.combined) {
            reSIDfp::CombinedWaveforms cw = reSIDfp::AVERAGE;
            if (cc.tuning.combined == CombinedWaves::Weak)
                cw = reSIDfp::WEAK;
            else if (cc.tuning.combined == CombinedWaves::Strong)
                cw = reSIDfp::STRONG;
            sid_.setCombinedWaveforms(cw);
        }
        if (modelChanged || was.tuning.filterCurve != cc.tuning.filterCurve) {
            if (is8580)
                sid_.setFilter8580Curve(std::clamp<double>(cc.tuning.filterCurve, 0.0, 1.0));
            else
                sid_.setFilter6581Curve(std::clamp<double>(cc.tuning.filterCurve, 0.0, 1.0));
        }
        if (modelChanged || was.tuning.dacLeakage != cc.tuning.dacLeakage)
            sid_.setDacLeakage(std::clamp<double>(cc.tuning.dacLeakage, 0.0, 1.0));
        if (modelChanged || was.tuning.digiOffset != cc.tuning.digiOffset)
            sid_.setOffset6581(std::clamp<double>(cc.tuning.digiOffset, 0.0, 1.0));
        if (modelChanged || was.tuning.filterEnabled != cc.tuning.filterEnabled)
            sid_.enableFilter(cc.tuning.filterEnabled);
        if (modelChanged || was.tuning.filterRange != cc.tuning.filterRange)
            sid_.setFilter6581Range(std::clamp<double>(cc.tuning.filterRange, 0.0, 1.0));
        if (modelChanged || was.tuning.oldCaps != cc.tuning.oldCaps)
            sid_.enableOld6581caps(cc.tuning.oldCaps);
    }

    void setSampling(double clockHz, SidResampling resampling, double sampleRate) override
    {
        sid_.setSamplingParameters(clockHz, resampling == SidResampling::Fast ? reSIDfp::DECIMATE : reSIDfp::RESAMPLE,
                                   sampleRate);
    }

    void reset() override { sid_.reset(); }
    void write(uint8_t reg, uint8_t v) override { sid_.write(reg, v); }
    uint8_t read(uint8_t reg) override { return sid_.read(reg); }
    uint8_t peek(uint8_t reg) const override { return sid_.peek(reg); }
    int clock(unsigned cycles, int16_t* buf) override { return sid_.clock(cycles, buf); }
    void setPaddle(uint8_t x, uint8_t y) override { sid_.setPaddle(x, y); }
    void setVoiceMask(uint8_t mask) override { sid_.setVoiceMask(mask); }

    int stateSize() const override { return sid_.stateSize(); }
    void saveState(char* out) const override { sid_.saveState(out, sid_.stateSize()); }
    bool restoreState(char* in, int size, const SidChipConfig& cc) override
    {
        if (size != sid_.stateSize())
            return false;
        sid_.restoreState(in, size);
        // The chip's saved state carries its 6581 range and caps too; the
        // machine's settings are the authority (they were applied before
        // this). Not the curve: setting it recomputes the cutoff voltage the
        // state just restored, which need not equal its formula at that
        // moment, and the restore already brings back the curve the
        // settings name.
        if (!sidVariantIs8580(cc.variant)) {
            sid_.setFilter6581Range(std::clamp<double>(cc.tuning.filterRange, 0.0, 1.0));
            sid_.enableOld6581caps(cc.tuning.oldCaps);
        }
        return true;
    }

private:
    reSIDfp::residfp sid_;
};

// ---------------------------------------------------------------- SIDLite

// SIDLite runs at 48 kHz at most. Above that it runs at the host rate
// divided by the smallest whole number that gets it there (96 kHz -> 48,
// 88.2 -> 44.1, 192 -> 48) and is upsampled by that number with linear
// interpolation: a whole ratio, so the sample count cannot drift.
//
// What SIDLite leaves to us is done here as reSIDfp does it: the POT
// registers, and the value a read of a write-only register finds on the data
// bus, which fades after a while.
class LiteChip final : public SidChip
{
public:
    SidEngine engine() const override { return SidEngine::Lite; }

    void tune(const SidChipConfig& cc, const SidChipConfig& was, bool valid) override
    {
        const bool is8580 = sidVariantIs8580(cc.variant);
        if (!valid || sidVariantIs8580(was.variant) != is8580) {
            sid_.setChipModel(is8580 ? SIDLite::SID::model_t::MOS8580 : SIDLite::SID::model_t::MOS6581);
            is8580_ = is8580;
        }
    }

    void setSampling(double clockHz, SidResampling, double sampleRate) override
    {
        up_ = std::max(1, static_cast<int>(std::ceil(sampleRate / 48000.0 - 1e-9)));
        const double internal = sampleRate / up_;
        sid_.setSamplingParameters(clockHz, static_cast<unsigned>(std::lround(std::max(8000.0, internal))));
    }

    void reset() override
    {
        sid_.reset();
        bus_ = 0;
        busTtl_ = 0;
        prev_ = 0;
    }

    void write(uint8_t reg, uint8_t v) override
    {
        sid_.write(reg & 0x1f, v);
        bus_ = v;
        busTtl_ = ttl();
    }

    uint8_t read(uint8_t reg) override
    {
        switch (reg & 0x1f) {
        case 0x19:
        case 0x1a:
        case 0x1b:
        case 0x1c:
            bus_ = peek(reg);
            busTtl_ = ttl();
            return bus_;
        default:
            // Reading a write-only register discharges the bus faster.
            busTtl_ /= 2;
            return bus_;
        }
    }

    uint8_t peek(uint8_t reg) const override
    {
        switch (reg & 0x1f) {
        case 0x19: return potX_;
        case 0x1a: return potY_;
        case 0x1b:
        case 0x1c: return static_cast<uint8_t>(sid_.read(reg & 0x1f));
        default: return bus_;
        }
    }

    int clock(unsigned cycles, int16_t* buf) override
    {
        if (busTtl_ > 0) {
            busTtl_ = cycles >= busTtl_ ? 0 : busTtl_ - cycles;
            if (busTtl_ == 0)
                bus_ = 0;
        }
        int out = 0;
        while (cycles) {
            // Small chunks, so tmp_ holds what one makes at any ratio: at
            // 48 kHz at most 75 samples, and 215 even unsampled (a ratio of
            // 0, one sample every 7 cycles).
            const unsigned n = std::min(cycles, 1500u);
            cycles -= n;
            const int got = sid_.clock(n, tmp_.data());
            if (up_ == 1) {
                std::memcpy(buf + out, tmp_.data(), static_cast<size_t>(got) * sizeof(int16_t));
                out += got;
                continue;
            }
            for (int k = 0; k < got; ++k) {
                const int s = tmp_[static_cast<size_t>(k)];
                for (int j = 1; j <= up_; ++j)
                    buf[out++] = static_cast<int16_t>(prev_ + (s - prev_) * j / up_);
                prev_ = static_cast<int16_t>(s);
            }
        }
        return out;
    }

    void setPaddle(uint8_t x, uint8_t y) override
    {
        potX_ = x;
        potY_ = y;
    }

    void setVoiceMask(uint8_t mask) override { sid_.setVoiceMask(mask & 0x07); }

    // A magic, SIDLite's own state, then ours: bus, its time to live, the
    // last internal sample (the interpolation's start).
    static constexpr int kExtra = 1 + 4 + 2;
    int stateSize() const override { return 4 + sid_.stateSize() + kExtra; }

    void saveState(char* out) const override
    {
        std::memcpy(out, kMagic, 4);
        const int n = sid_.stateSize();
        sid_.saveState(reinterpret_cast<unsigned char*>(out + 4));
        char* p = out + 4 + n;
        std::memcpy(p, &bus_, 1);
        std::memcpy(p + 1, &busTtl_, 4);
        std::memcpy(p + 5, &prev_, 2);
    }

    bool restoreState(char* in, int size, const SidChipConfig&) override
    {
        if (size != stateSize() || std::memcmp(in, kMagic, 4) != 0)
            return false;
        const int n = sid_.stateSize();
        if (!sid_.restoreState(reinterpret_cast<const unsigned char*>(in + 4), n))
            return false;
        const char* p = in + 4 + n;
        std::memcpy(&bus_, p, 1);
        std::memcpy(&busTtl_, p + 1, 4);
        std::memcpy(&prev_, p + 5, 2);
        return true;
    }

private:
    static constexpr char kMagic[4] = {'S', 'L', 'T', '1'};

    // The data bus holds a written value this many cycles (reSIDfp's).
    uint32_t ttl() const { return is8580_ ? 0xa2000u : 0x1d00u; }

    SIDLite::SID sid_;
    std::array<int16_t, 256> tmp_{};
    int up_ = 1;
    int16_t prev_ = 0;
    uint8_t potX_ = 0xff, potY_ = 0xff;
    uint8_t bus_ = 0;
    uint32_t busTtl_ = 0;
    bool is8580_ = false;
};

} // namespace

std::unique_ptr<SidChip> makeSidChip(SidEngine engine)
{
    if (engine == SidEngine::Lite)
        return std::make_unique<LiteChip>();
    return std::make_unique<ReSidFpChip>();
}

} // namespace sidmidi::emu
