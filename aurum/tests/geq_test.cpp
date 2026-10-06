// Accuracy test for the attenuation GEQ fit: relative T60 error per case.
#include "dsp/Geq.h"
#include <cstdio>
#include <cstdlib>
using namespace aurum::dsp;

static double t60Curve(double f, int variant)
{
    double l = std::log2(f / 1000.0);
    switch (variant) {
    case 0: return 2.0 * std::pow(2.0, -0.6 * std::tanh(l - 1.5));                          // HF damping
    case 1: return 1.5 * std::pow(2.0, 1.0 * std::exp(-l * l / 0.5));                       // bell boost
    case 2: return 3.0 * std::pow(2.0, -2.0 * std::exp(-(l + 1) * (l + 1) / 0.08));         // narrow dip
    case 3: return 0.3 * std::pow(2.0, 0.8 * std::tanh(-(l + 2)) - 1.5 * std::tanh(l - 2)); // extreme tilt
    default: return 10.0 * std::pow(2.0, -1.5 * std::tanh(l - 1.0));                         // cathedral
    }
}

int main(int argc, char** argv)
{
    double fs = argc > 1 ? atof(argv[1]) : 48000.0;
    double qf = argc > 2 ? atof(argv[2]) : 0.5;
    GeqDesigner d;
    d.qFactor = qf;
    d.prepare(fs);
    printf("fs=%.0f qf=%.2f bands=%d points=%d\n", fs, qf, d.numBands(), d.numPoints());
    for (int refine = 0; refine < 3; ++refine) {
        double gWorst = 0, gMean = 0; int gCnt = 0;
        for (int v = 0; v < 5; ++v) {
            double shape[GeqDesigner::kMaxPoints];
            for (int m = 0; m < d.numPoints(); ++m) shape[m] = -60.0 / (fs * t60Curve(d.pointFreq(m), v));
            d.setShape(shape);
            for (double delayMs : {3.0, 20.0, 60.0, 150.0}) {
                double dsamp = delayMs * 1e-3 * fs;
                double g[GeqDesigner::kMaxBands], bb;
                d.design(dsamp, g, bb, refine);
                double worst = 0, sum = 0, wf = 0; int cnt = 0;
                for (double f = 25; f < std::min(20000.0, 0.45 * fs); f *= 1.02) {
                    double tt = -60.0 * dsamp / (fs * t60Curve(f, v));
                    if (tt < -40) continue;
                    double rel = std::fabs(d.realisedDbAt(g, bb, f) - tt) / std::fabs(tt);
                    if (rel > worst) { worst = rel; wf = f; }
                    sum += rel; ++cnt;
                }
                if (cnt == 0) continue;
                if (argc > 3) printf("  ref=%d v=%d d=%4.0fms mean=%.2f%% worst=%.2f%% @%.0fHz\n", refine, v, delayMs, 100*sum/cnt, 100*worst, wf);
                if (v != 2 && v != 3) { gWorst = std::max(gWorst, worst); gMean += sum / cnt; ++gCnt; }
            }
        }
        printf("refine=%d realistic curves: mean %.2f%% worst %.2f%%\n", refine, 100 * gMean / gCnt, 100 * gWorst);
    }
}
