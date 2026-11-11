#pragma once

// Up to eight SID chips, each with its own model, variant and address, all on
// one engine: reSIDfp (the reference) or SIDLite (light, for slower
// machines; see SidChip.h).
//
// The chips are clocked lazily: a chip is brought up to the machine's cycle
// whenever its registers are touched and at the end of every audio block,
// producing samples at the host rate (reSIDfp: through its resampler). Each
// chip writes into its own FIFO; the resamplers do not land on exactly the
// same sample count every block, so the FIFOs are read with a little latency
// and nobody hears the difference.
//
// Voice outputs: a SID mixes its three voices through one analogue filter, so
// there is no single voice to tap after it. With voice outputs on, each chip
// gets three more instances of its engine (taps) that see every register write but
// hear only one voice each (the vendored voice mask, patch 0003): each voice
// goes through its own filter, mixer and volume. On a 6581, whose filter
// distorts the sum, the three together are close to the chip's output but
// not equal to it. A chip with taps costs about four times as much.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace resident::emu {

class SidChip;

// The SID engine of the whole machine. Accurate: reSIDfp, every control
// applies. Light: SIDLite, about 30 times cheaper per SID; of the tuning only
// the model (6581 or 8580) applies, and a register write lands on the next
// output sample.
enum class SidEngine : uint8_t { ReSidFp = 0, Lite };

enum class SidVariant : uint8_t
{
    MOS6581 = 0, // a generic 6581 (reSIDfp's defaults)
    MOS6581R3,
    MOS6581R4,
    MOS6581R4AR,
    MOS8580R5,
    Count
};

const char* sidVariantName(SidVariant v);
bool sidVariantIs8580(SidVariant v);

enum class CombinedWaves : uint8_t { Average = 0, Weak, Strong };

// What a variant sets; every field is a control the user can override.
struct SidTuning
{
    float filterCurve = 0.5f;  // 6581: 0 dark .. 1 bright; 8580: same direction
    CombinedWaves combined = CombinedWaves::Average;
    float dacLeakage = 1.0f;   // 0 none .. 1 standard
    float digiOffset = 1.0f;   // 6581 only: 0 average digis .. 1 loud
    bool filterEnabled = true;
    // 6581 only, per chip (vendored reSIDfp patch 0002): the filter's range,
    // 0 dark .. 1 bright (19/39 is reSIDfp's own default), and the 2200 pF
    // filter caps of the early ASSY 326298 boards.
    float filterRange = 19.0f / 39.0f;
    bool oldCaps = false;
};

// How each chip's output gets from the SID clock to the host rate. Sinc is
// reSIDfp's two-pass sinc resampler, the reference; Fast takes the nearest
// sample (no anti-aliasing), much cheaper with many SIDs, and it aliases.
enum class SidResampling : uint8_t { Sinc = 0, Fast };

SidTuning sidVariantTuning(SidVariant v);

struct SidChipConfig
{
    uint16_t address = 0xd400;
    SidVariant variant = SidVariant::MOS6581R4AR;
    SidTuning tuning = sidVariantTuning(SidVariant::MOS6581R4AR);
};

struct SidBankConfig
{
    int count = 1;
    SidEngine engine = SidEngine::ReSidFp;
    SidResampling resampling = SidResampling::Sinc;
    bool voiceOutputs = false; // three taps per chip, one per voice
    std::array<SidChipConfig, 8> chips{};
    // $D400, $D420, $D440 ... -- SID-Wizard's defaults for its 2/3/4SID builds.
    SidBankConfig()
    {
        for (int i = 0; i < 8; ++i)
            chips[static_cast<size_t>(i)].address = static_cast<uint16_t>(0xd400 + 0x20 * i);
    }
};

class SidBank
{
public:
    static constexpr int kMaxChips = 8;
    static constexpr int kVoices = 3;
    static constexpr size_t kFifoSize = 8192;

    SidBank();
    ~SidBank();

    // Applies a configuration. Builds chips that do not exist yet; that is
    // slow the first time reSIDfp builds its tables, so do it off the audio
    // thread when possible. Keeps the register state of chips that survive.
    void configure(const SidBankConfig& cfg, double clockHz, double sampleRate, uint64_t clk);
    const SidBankConfig& config() const { return cfg_; }
    int count() const { return cfg_.count; }

    void reset(uint64_t clk);
    // After a reset and its registers written back on a running machine (a
    // snapshot this build cannot restore, a new engine): the writes step the
    // output's DC, which takes half a second to die away through the C64's
    // output filter -- a thump. This runs every chip that long, unheard and
    // without moving the machine's time. Off the audio thread.
    void settle();

    // Address decoding. Returns the chip at `addr`, or -1. SID 1 mirrors
    // through $D400-$D7FF wherever no other chip is placed.
    int chipAt(uint16_t addr) const;
    uint8_t read(int chip, uint8_t reg, uint64_t clk);
    void write(int chip, uint8_t reg, uint8_t v, uint64_t clk);
    uint8_t peek(int chip, uint8_t reg) const;
    uint8_t lastWritten(int chip, uint8_t reg) const { return regs_[chip][reg & 0x1f]; }
    void setLastWritten(int chip, uint8_t reg, uint8_t v) { regs_[chip][reg & 0x1f] = v; }

    // While skipping (warp), catching up moves the chips' time without
    // clocking them: nothing is produced, nothing costs.
    void setSkipping(bool on) { skipping_ = on; }

    // Chips currently skipped as dormant (for tests and the GUI).
    int dormantCount(uint64_t clk) const;

    // POT lines of SID 1 (paddles, mouse).
    void setPots(uint8_t x, uint8_t y);

    // Brings every chip up to `clk`.
    void sync(uint64_t clk);
    // Discards everything produced so far (warp, mute).
    void discard();
    // Samples a chip has ready.
    size_t available(int chip) const { return chips_[chip].fifo.available(); }
    // Pops `n` samples of a chip as floats (-1..1); pads with silence.
    void pop(int chip, float* out, size_t n) { chips_[chip].fifo.pop(out, n); }
    // The same for one voice's tap, while voice outputs are on.
    bool hasVoices(int chip) const { return chips_[chip].taps[0] != nullptr; }
    size_t availableVoice(int chip, int v) const { return chips_[chip].voiceFifo[v].available(); }
    void popVoice(int chip, int v, float* out, size_t n) { chips_[chip].voiceFifo[v].pop(out, n); }

    // The samples waiting in a chip's FIFO, and putting them back (state).
    // setPending also fills the chip's voice queues with as much silence;
    // setPendingVoice then puts back what they held.
    std::vector<int16_t> pending(int chip) const;
    void setPending(int chip, const std::vector<int16_t>& samples);
    std::vector<int16_t> pendingVoice(int chip, int v) const;
    void setPendingVoice(int chip, int v, const std::vector<int16_t>& samples);

    // Snapshot of the chips' internal state (not portable across builds).
    std::vector<char> saveState() const;
    bool restoreState(const std::vector<char>& data, uint64_t clk);

private:
    struct Fifo
    {
        std::vector<int16_t> buf;
        size_t rd = 0, wr = 0;
        void push(int16_t s)
        {
            const size_t next = (wr + 1) % kFifoSize;
            if (next == rd)
                rd = (rd + 1) % kFifoSize; // overflow: drop the oldest
            buf[wr] = s;
            wr = next;
        }
        size_t available() const { return (wr + kFifoSize - rd) % kFifoSize; }
        void pop(float* out, size_t n);
        void clear() { rd = wr = 0; }
    };
    struct Chip
    {
        std::unique_ptr<SidChip> sid;
        // Voice outputs: one chip per voice, hearing only that voice.
        std::array<std::unique_ptr<SidChip>, kVoices> taps;
        std::array<Fifo, kVoices> voiceFifo;
        uint64_t clk = 0;
        // A chip nobody has written to since its reset is dormant once its
        // output has settled: it is not clocked at all, it produces silence.
        // On a machine with eight SIDs and a program that uses one, that is
        // most of the cost of the other seven.
        bool written = false;
        uint64_t resetClk = 0;
        double silentFrac = 0.0;
        Fifo fifo;
    };
    void catchUp(Chip& c, uint64_t clk);
    void settleChip(Chip& c);
    void pushSilence(Chip& c, uint64_t cycles);
    void applyTuning(int i, bool force);
    void setSampling(SidChip& sid) const;
    // Builds a chip's taps as copies of the chip, each hearing one voice.
    void createTaps(int i);

    SidBankConfig cfg_;
    std::array<SidChipConfig, kMaxChips> applied_{};
    std::array<bool, kMaxChips> appliedValid_{};
    std::array<Chip, kMaxChips> chips_;
    std::array<std::array<uint8_t, 32>, kMaxChips> regs_{};
    std::vector<int16_t> scratch_;
    double clockHz_ = 985248.0, sampleRate_ = 48000.0;
    bool skipping_ = false;
    uint8_t potX_ = 0xff, potY_ = 0xff;
};

} // namespace resident::emu
