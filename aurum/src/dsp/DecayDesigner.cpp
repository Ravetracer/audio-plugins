#include "DecayDesigner.h"

#include <chrono>

namespace aurum::dsp {

DecayDesigner::DecayDesigner()
    : requests_(std::make_unique<TripleBuffer<DecayRequest>>()),
      results_(std::make_unique<TripleBuffer<DecayCoeffSet>>())
{
    for (Context* c : {&sync_, &worker_})
        c->gains = std::make_unique<double[]>(static_cast<size_t>(kMaxDecayLanes) * GeqDesigner::kMaxBands);
}

DecayDesigner::~DecayDesigner() { stopWorker(); }

void DecayDesigner::prepare(double sampleRate)
{
    fs_ = sampleRate;
    sync_.geq.prepare(sampleRate);
    worker_.geq.prepare(sampleRate);
}

void DecayDesigner::startWorker()
{
    if (running_.exchange(true))
        return;
    thread_ = std::thread([this] { workerLoop(); });
}

void DecayDesigner::stopWorker()
{
    if (!running_.exchange(false))
        return;
    wake_.release();
    if (thread_.joinable())
        thread_.join();
}

void DecayDesigner::post(const DecayRequest& req)
{
    requests_->writeSlot() = req;
    requests_->publish();
    wake_.release();
}

const DecayCoeffSet* DecayDesigner::poll()
{
    if (!results_->fetch())
        return nullptr;
    return &results_->readSlot();
}

void DecayDesigner::workerLoop()
{
    while (running_.load(std::memory_order_acquire))
    {
        wake_.acquire();
        if (!running_.load(std::memory_order_acquire))
            break;
        if (!requests_->fetch())
            continue;
        const DecayRequest& req = requests_->readSlot();
        computeWith(worker_, req, results_->writeSlot());
        results_->publish();
    }
}

void DecayDesigner::computeWith(Context& ctx, const DecayRequest& req, DecayCoeffSet& out)
{
    GeqDesigner& geq_ = ctx.geq;
    const int lanes = std::min(req.numLanes, kMaxDecayLanes);
    const int K = geq_.numBands();
    out.tag = req.tag;
    out.numLanes = lanes;
    out.numBands = K;

    auto setIdentity = [&](int lane) {
        for (int k = 0; k < K; ++k)
        {
            out.a1[k][lane] = 1.0f;
            out.a2[k][lane] = out.a3[k][lane] = 0.0f;
            out.m0[k][lane] = 1.0f;
            out.m1[k][lane] = out.m2[k][lane] = 0.0f;
        }
        out.broadband[lane] = 1.0f;
    };

    if (req.freeze || lanes == 0)
    {
        for (int i = 0; i < lanes; ++i)
            setIdentity(i);
        return;
    }

    double shape[GeqDesigner::kMaxPoints];
    for (int m = 0; m < geq_.numPoints(); ++m)
        shape[m] = -60.0 / (fs_ * req.model.t60At(geq_.pointFreq(m)));
    geq_.setShape(shape);

    double* gains = ctx.gains.get();
    double bb[kMaxDecayLanes];
    geq_.designLines(req.delays, lanes, gains, bb);

    for (int i = 0; i < lanes; ++i)
    {
        double* g = gains + static_cast<size_t>(i) * GeqDesigner::kMaxBands;
        // Stability guard: bells are transparent at DC and Nyquist, so the
        // response there is the broadband gain plus the edge shelf. Keep both
        // strictly below unity.
        double dc = bb[i], nyq = bb[i];
        for (int k = 0; k < K; ++k)
        {
            if (geq_.bandKind(k) == GeqDesigner::Kind::LowShelf)
                dc += g[k];
            else if (geq_.bandKind(k) == GeqDesigner::Kind::HighShelf)
                nyq += g[k];
        }
        const double edge = std::max(dc, nyq);
        if (edge > -1e-6)
            bb[i] -= edge + 1e-6;
        for (int k = 0; k < K; ++k)
        {
            const double f = geq_.bandFreq(k);
            const double q = geq_.bandQ(k);
            SvfCoeffs c;
            switch (geq_.bandKind(k))
            {
            case GeqDesigner::Kind::Bell: c = SvfCoeffs::bell(f, q, g[k], fs_); break;
            case GeqDesigner::Kind::LowShelf: c = SvfCoeffs::lowShelf(f, q, g[k], fs_); break;
            case GeqDesigner::Kind::HighShelf: c = SvfCoeffs::highShelf(f, q, g[k], fs_); break;
            }
            out.a1[k][i] = c.a1;
            out.a2[k][i] = c.a2;
            out.a3[k][i] = c.a3;
            out.m0[k][i] = c.m0;
            out.m1[k][i] = c.m1;
            out.m2[k][i] = c.m2;
        }
        out.broadband[i] = static_cast<float>(std::pow(10.0, bb[i] / 20.0));
    }
}

} // namespace aurum::dsp
