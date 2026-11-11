#pragma once

// Runs the C64 against the host's clock.
//
// One process() call is one host block: the machine runs the block's worth of
// cycles, MIDI bytes from the host enter the cartridge at the cycle their
// sample offset corresponds to, the SIDs are drained into the outputs, and
// what the program transmitted comes back with sample offsets. The plugin and
// the headless runner both drive the machine through this class, so the
// runner tests the plugin's own path.
//
// Booting runs in warp: up to kWarpFactor blocks' worth of cycles per block,
// a fixed amount so offline renders stay deterministic, with the audio muted
// until the program has been started.

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "emu/C64.h"

namespace resident {

struct MidiEvent
{
    uint32_t offset = 0; // sample offset within the block
    uint8_t size = 0;
    uint8_t data[3] = {};
    // A longer message (SysEx, e.g. ASID) lives elsewhere for the block.
    const uint8_t* ext = nullptr;
    uint32_t extSize = 0;
};

struct SidMix
{
    float level = 1.0f; // linear
    float pan = 0.0f;   // -1 left .. +1 right
};

class Engine
{
public:
    static constexpr int kWarpFactor = 24;      // offline
    static constexpr int kWarpFactorMax = 64;   // realtime, if the time allows
    static constexpr size_t kSidLatency = 16; // samples kept in each SID FIFO

    Engine();

    emu::C64& machine() { return c64_; }
    const emu::C64& machine() const { return c64_; }

    void setSampleRate(double rate);
    // Sizes the mix buffer for the largest block, so process() never grows it.
    void setMaxFrames(uint32_t frames)
    {
        if (scratch_.size() < frames)
            scratch_.resize(frames);
    }
    double sampleRate() const { return rate_; }
    // Re-applies the SID configuration (models, addresses, count).
    void configureSids(const emu::SidBankConfig& cfg);
    void setStandard(emu::VideoStandard std);

    // Whether anything shows the screen (the plugin's window).
    std::atomic<bool> videoWanted{true};

    // Mixing.
    std::array<SidMix, emu::SidBank::kMaxChips> mix{};
    float masterGain = 1.0f;

    struct Outputs
    {
        float* left = nullptr;
        float* right = nullptr;
        float* sid[emu::SidBank::kMaxChips] = {}; // optional, mono per SID
        // Optional, mono per voice; silent unless voice outputs are on.
        float* voice[emu::SidBank::kMaxChips][emu::SidBank::kVoices] = {};
    };

    // `events` sorted by offset.
    void process(uint32_t frames, const MidiEvent* events, size_t numEvents, const Outputs& out,
                 std::vector<MidiEvent>& midiOut);

    // Warp when asked (the Warp button), and on its own until the program has
    // started and while the true 1541 works -- each of those can be switched
    // off, and an automatic warp in progress can be stopped by setWarp(false).
    bool warping() const { return warp_ || (autoWarping() && !holdAuto_); }
    bool autoWarping() const
    {
        return (warpOnBoot && (c64_.autostartPending() || c64_.booting())) ||
               (warpOnDiskAccess && c64_.trueDriveActive() && c64_.drive1541.motorOn());
    }
    bool warpOnBoot = true;
    bool warpOnDiskAccess = true;
    // Realtime: warp spends at most part of each block's time, measured on the
    // clock. Offline (renders, tests): a fixed amount, so results repeat.
    bool realtime = true;
    // Off also stops an automatic warp that is running, until it would end.
    void setWarp(bool on)
    {
        warp_ = on;
        holdAuto_ = !on && autoWarping();
    }
    // After a power cycle or a reload: discard queued audio and re-prime, and
    // let an automatic warp the Warp button stopped run again.
    void resync();
    // What lies between the machine and the outputs (the cycle fraction, the
    // samples queued per SID), so a restored project continues seamlessly.
    std::vector<uint8_t> saveAudioState() const;
    bool loadAudioState(const std::vector<uint8_t>& data);

    // The latest finished frame, for the GUI: copied under a seqlock-free
    // triple buffer. frameSerial() changes when a new frame is available.
    uint64_t frameSerial() const { return frameSerial_.load(std::memory_order_acquire); }
    // Copies the newest frame into `dst` (kWidth*kHeight bytes). Returns its serial.
    uint64_t copyFrame(uint8_t* dst);

    // Activity, for the GUI's indicators (read from any thread).
    std::atomic<uint32_t> midiInCount{0}, midiOutCount{0};
    std::atomic<bool> driveLed{false};

private:
    void resyncAudio(); // discard queued audio and re-prime
    void publishFrame();
    void runTo(uint64_t clk);

    emu::C64 c64_;
    double rate_ = 48000.0;
    double cyclesPerSample_ = 985248.0 / 48000.0;
    double clkFrac_ = 0.0;
    bool warp_ = false;
    bool holdAuto_ = false; // an automatic warp the user stopped
    bool wasWarping_ = true;
    uint64_t lastFrame_ = ~0ull;
    std::vector<float> scratch_;
    std::vector<emu::MidiByte> txBytes_;

    // Triple buffer: the audio thread writes into `back`, publishes by
    // swapping with `middle`; the GUI swaps `front` with `middle`.
    std::array<std::vector<uint8_t>, 3> frames_;
    std::atomic<int> middle_{1};
    int back_ = 0, front_ = 2;
    std::atomic<bool> fresh_{false};
    std::atomic<uint64_t> frameSerial_{0};
};

} // namespace resident
