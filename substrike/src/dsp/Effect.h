#pragma once

#include <array>
#include <cstddef>

namespace substrike::dsp {

// The slot types, in the order the Type parameter lists them. State stores the
// label, so the list may grow at the end.
enum class SlotType : int
{
    Off = 0,
    Distortion,
    Clipper,
    Wavefolder,
    Bitcrush,
    Filter,
    Eq,
    Compressor,
    Transient,
    Gate,
    Reverb,
    Delay,
    Warp,
    Smear,
    Ring,
    Stereo,
    Utility,
    Limiter,
    Comb,
};
constexpr int kNumSlotTypes = 19;

// Which part of the spectrum a slot processes. The rest passes by it.
enum class Band : int
{
    Full = 0,
    Low,
    Mid,
    High,
    LowMid,
    MidHigh,
};

constexpr int kNumSlots = 6;
constexpr int kSlotValues = 6;

// One slot as the parameters set it. `v` holds A-F in the plain units the
// type gives them (see the shape table in plugin/Params.cpp); what each one
// means is written beside each effect.
struct SlotParams
{
    SlotType type = SlotType::Off;
    Band band = Band::Full;
    double mix = 1.0;
    bool bypass = false;
    std::array<double, kSlotValues> v{};
};

// A choice (a model, a mode) jumps; everything else glides.
bool slotValueIsChoice(SlotType t, int i);
// The drive group runs inside the oversampler.
bool slotIsOversampled(SlotType t);

class Effect
{
public:
    virtual ~Effect() = default;
    virtual void prepare(double sampleRate) = 0;
    // Clears the state. Memory from attach() is cleared by the slot.
    virtual void reset() = 0;
    // The six values, in plain units. Called every 16 samples; an effect
    // that derives something expensive checks for a change itself.
    virtual void set(const double* v) = 0;
    // The lane fired a hit (the master: a note arrived).
    virtual void hit() {}
    virtual void process(float* l, float* r, int n) = 0;

    // Delay memory, in floats, at this rate. A slot runs one type at a time,
    // so every type of a slot shares one block, allocated with the rate and
    // handed over with attach() after prepare().
    virtual size_t memory(double /*sampleRate*/) const { return 0; }
    virtual void attach(float* /*mem*/) {}
    // How long the output may stay silent while the effect still holds a
    // sound (the gap before an echo), in samples. The lane waits this much
    // longer before it decides its chain has rung out.
    virtual int silentHold() const { return 0; }
    // Beats per minute, for the synced times. Set before set().
    virtual void setTempo(double /*bpm*/) {}
};

// A synced time as a note value: the choices of a Sync letter, and their
// length in quarter notes (0 for Free, which leaves the time in ms).
double syncBeats(int choice);

} // namespace substrike::dsp
