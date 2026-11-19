#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

// A WebAssembly build without wasi-threads has no std::thread: there the
// designer has no worker and every request is computed synchronously.
#if defined(__wasm__) && !defined(_REENTRANT)
#define AURUM_DECAY_WORKER 0
#else
#define AURUM_DECAY_WORKER 1
#include <thread>
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <semaphore>
#endif
#endif

#include "DecayModel.h"
#include "Geq.h"

namespace aurum::dsp {

#if AURUM_DECAY_WORKER && defined(__APPLE__)
// What std::binary_semaphore does here, for macOS before 11, where libc++ has
// no semaphores: a dispatch semaphore, which signals without a lock, so
// post() stays safe on the audio thread. Surplus signals only make the worker
// look for a request once more.
class WakeSignal
{
public:
    WakeSignal() : sem_(dispatch_semaphore_create(0)) {}
    ~WakeSignal() { dispatch_release(sem_); }
    WakeSignal(const WakeSignal&) = delete;
    WakeSignal& operator=(const WakeSignal&) = delete;
    void release() { dispatch_semaphore_signal(sem_); }
    void acquire() { dispatch_semaphore_wait(sem_, DISPATCH_TIME_FOREVER); }

private:
    dispatch_semaphore_t sem_;
};
#endif

// Lock-free single-producer/single-consumer triple buffer.
template <typename T> class TripleBuffer
{
public:
    T& writeSlot() { return slots_[back_]; }
    void publish() { back_ = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel) & kIndex; }
    bool fetch()
    {
        if (!(middle_.load(std::memory_order_acquire) & kDirty))
            return false;
        front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndex;
        return true;
    }
    const T& readSlot() const { return slots_[front_]; }

private:
    static constexpr int kIndex = 3;
    static constexpr int kDirty = 4;
    T slots_[3]{};
    std::atomic<int> middle_{1};
    int front_ = 0;
    int back_ = 2;
};

constexpr int kMaxDecayLanes = 32;

struct DecayRequest
{
    DecayModel model;
    double delays[kMaxDecayLanes]{}; // segment lengths in samples
    int numLanes = 0;
    bool freeze = false;
    int tag = 0; // identifies the structure the delays belong to
};

// Ready-to-use SVF coefficients for every lane.
struct DecayCoeffSet
{
    int tag = 0;
    int numLanes = 0;
    int numBands = 0;
    float a1[GeqDesigner::kMaxBands][kMaxDecayLanes];
    float a2[GeqDesigner::kMaxBands][kMaxDecayLanes];
    float a3[GeqDesigner::kMaxBands][kMaxDecayLanes];
    float m0[GeqDesigner::kMaxBands][kMaxDecayLanes];
    float m1[GeqDesigner::kMaxBands][kMaxDecayLanes];
    float m2[GeqDesigner::kMaxBands][kMaxDecayLanes];
    float broadband[kMaxDecayLanes];
};

// Turns a decay model plus the segment lengths of a reverb structure into
// attenuation filter coefficients. Runs on a worker thread during realtime
// playback (the fit costs a few hundred microseconds) and synchronously for
// offline rendering and activation.
class DecayDesigner
{
public:
    DecayDesigner();
    ~DecayDesigner();

    void prepare(double sampleRate);
    void startWorker();
    void stopWorker();
    // False when built without threads: post() must not be used then.
    static constexpr bool hasWorker() { return AURUM_DECAY_WORKER != 0; }

    // Synchronous computation on the calling (audio) thread. Uses its own
    // designer state, so it may run while the worker is busy.
    void compute(const DecayRequest& req, DecayCoeffSet& out) { computeWith(sync_, req, out); }

    // Audio thread: queue a request for the worker.
    void post(const DecayRequest& req);
    // Audio thread: returns the newest finished set, or nullptr.
    const DecayCoeffSet* poll();

    const GeqDesigner& geq() const { return sync_.geq; }

private:
    struct Context
    {
        GeqDesigner geq;
        std::unique_ptr<double[]> gains;
    };

    void workerLoop();
    void computeWith(Context& ctx, const DecayRequest& req, DecayCoeffSet& out);

    Context sync_, worker_;
    double fs_ = 48000.0;
    std::unique_ptr<TripleBuffer<DecayRequest>> requests_;
    std::unique_ptr<TripleBuffer<DecayCoeffSet>> results_;
#if AURUM_DECAY_WORKER
#if defined(__APPLE__)
    WakeSignal wake_;
#else
    std::binary_semaphore wake_{0};
#endif
    std::atomic<bool> running_{false};
    std::thread thread_;
#endif
};

} // namespace aurum::dsp
