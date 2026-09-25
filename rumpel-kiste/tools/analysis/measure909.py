#!/usr/bin/env python3
"""Measurements of a TR-909's toms and rim shot, from recordings of one.

The numbers in drums.cpp marked "measured" come from here. The recordings are
a commercial sample pack (96 kHz, 24 bit, individual outputs, every knob in
10 % steps) kept in !dev/TR-909-96kHz-24bit and not ours to redistribute.

    measure909.py toms    [--pack DIR]   sweep, envelopes and partials per tom
    measure909.py rim     [--pack DIR]   the rim shot model fitted to its waveform
    measure909.py bd      [--pack DIR]   the kick's sweep, hold and fall
    measure909.py sd      [--pack DIR]   the snare's partials and noise tail
    measure909.py clap    [--pack DIR]   burst onsets and the tail
    measure909.py hats    [--pack DIR]   decays, the ROM clock and the run
    measure909.py cymbals [--pack DIR]   the ROM clock over Tune
    measure909.py levels  [--pack DIR]   accent spans and every voice against the kick
    measure909.py render FILE.wav        run the tom measurements on a render

numpy throughout; only the rim shot's fit needs scipy.
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', '..', 'shared', 'tools', 'analysis'))
from wavio import read_wav  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PACK = os.path.join(HERE, '..', '..', '!dev', 'TR-909-96kHz-24bit')
TOMS = ['LowTom', 'MidTom', 'HiTom']
# Each tom's settled pitch at Tune 50 %, from the partial fit below; used to
# seed the searches.
SETTLED = {'LowTom': 85.6, 'MidTom': 106.7, 'HiTom': 125.3}
# The partials a tom has, relative to the pitch heard: C19's oscillator a fifth
# under it, the pitch, harmonics, and C20's oscillator.
PARTIALS = [2 / 3, 1, 4 / 3, 2, 3, 2.75 * 2 / 3]


def load(path):
    x, sr = read_wav(path)
    if x.ndim > 1:
        x = x[:, 0]
    onset = int(np.argmax(np.abs(x) > 0.02 * np.abs(x).max()))
    return x[max(onset - 2, 0):], sr


def fit_partials(seg, sr, f):
    """Least-squares amplitudes of PARTIALS over `seg`, and the residual."""
    t = np.arange(len(seg)) / sr
    basis = []
    for h in PARTIALS:
        basis += [np.cos(2 * np.pi * h * f * t), np.sin(2 * np.pi * h * f * t)]
    basis = np.array(basis).T
    c = np.linalg.lstsq(basis, seg, rcond=None)[0]
    r = seg - basis @ c
    return np.sum(r ** 2) / np.sum(seg ** 2), [np.hypot(c[2 * i], c[2 * i + 1]) for i in range(len(PARTIALS))]


def best_pitch(seg, sr, guess, span=0.07, n=60):
    grid = guess * np.exp(np.linspace(np.log(1 - span), np.log(1 + span), n))
    return grid[int(np.argmin([fit_partials(seg, sr, g)[0] for g in grid]))]


def sweep(x, sr, settled):
    """The pitch over the first 150 ms, from zero crossings of the band around
    the pitch, averaged over three periods (the partial a fifth under beats
    with it over exactly three)."""
    n = int(0.2 * sr)
    spec = np.fft.rfft(x[:n])
    f = np.fft.rfftfreq(n, 1 / sr)
    spec[(f < 0.8 * settled) | (f > 1.7 * settled)] = 0
    y = np.fft.irfft(spec, n)
    zc = np.where((y[:-1] < 0) & (y[1:] >= 0))[0]
    tz = (zc + y[zc] / (y[zc] - y[zc + 1])) / sr
    out = []
    for i in range(0, min(len(tz) - 3, 15), 3):
        out.append(((tz[i] + tz[i + 3]) / 2, 3 / (tz[i + 3] - tz[i]) / settled))
    return out


def envelopes(x, sr, settled, times):
    """Amplitudes of every partial at `times`, 40 ms windows."""
    rows = []
    for t in times:
        w = int(0.04 * sr)
        i = max(int(t * sr) - w // 2, 0)
        seg = x[i:i + w]
        guess = settled * (1 + 0.26 * np.exp(-t / 0.03) + 0.12 * np.exp(-t / 0.22))
        rows.append(fit_partials(seg, sr, best_pitch(seg, sr, guess))[1])
    return np.array(rows)


def fit_main(times, a):
    """The pitch partial as a * tanh(h e^(-t/tau)) / tanh(h), by a coarse grid
    on log amplitude."""
    ok = a > a.max() * 0.003
    best = None
    for tau in np.linspace(0.02, 0.2, 181):
        for h in np.exp(np.linspace(np.log(0.3), np.log(12), 60)):
            m = np.tanh(h * np.exp(-times[ok] / tau)) / np.tanh(h)
            k = np.exp(np.mean(np.log(a[ok]) - np.log(m)))
            e = np.sum((np.log(a[ok]) - np.log(k * m)) ** 2)
            if best is None or e < best[0]:
                best = (e, k, tau, h)
    return best[1:]


def measure_tom(path, settled):
    x, sr = load(path)
    times = np.array([0.02, 0.03, 0.04, 0.05, 0.06, 0.08, 0.1, 0.13, 0.16, 0.2, 0.25, 0.3, 0.35, 0.4, 0.5, 0.6])
    env = envelopes(x, sr, settled, times)
    a, tau, h = fit_main(times, env[:, 1])
    i = list(times).index(0.1)
    return {
        'tau': tau, 'knee': h,
        'under_at_100ms_db': 20 * np.log10(env[i, 0] / env[i, 1]),
        'top_at_20ms': env[0, 5] / env[0, 1],
        'sweep': sweep(x, sr, settled),
    }


def cmd_toms(pack):
    for tom in TOMS:
        print('== %s' % tom)
        print('  decay   tau ms  knee  under@100ms  top@20ms')
        for d in range(0, 101, 10):
            r = measure_tom(os.path.join(pack, tom, '%s909-tune050-decay%03d.wav' % (tom, d)), SETTLED[tom])
            print('  %3d %%  %6.1f  %5.2f   %5.1f dB    %.2f' % (d, r['tau'] * 1000, r['knee'],
                                                          r['under_at_100ms_db'], r['top_at_20ms']))
        r = measure_tom(os.path.join(pack, tom, '%s909-tune050-decay090.wav' % tom), SETTLED[tom])
        print('  sweep (t ms: pitch / settled):', ' '.join('%.0f:%.3f' % (t * 1000, v) for t, v in r['sweep']))


def cmd_render(path):
    base = os.path.basename(path)
    tom = next((t for t in TOMS if base.startswith(t)), 'LowTom')
    r = measure_tom(path, SETTLED[tom])
    print('%s: tau %.1f ms, knee %.2f, under@100ms %.1f dB, top@20ms %.2f' % (
        base, r['tau'] * 1000, r['knee'], r['under_at_100ms_db'], r['top_at_20ms']))
    print('  sweep:', ' '.join('%.0f:%.3f' % (t * 1000, v) for t, v in r['sweep']))


# ------------------------------------------------------------------ rim shot

def cmd_rim(pack):
    from scipy.optimize import least_squares
    from scipy.signal import bilinear, lfilter

    # The three bridged-T networks: C, R (across), r (to ground).
    nets = [(0.01e-6, 470e3, 2.2e3), (0.027e-6, 330e3, 2.2e3), (0.0047e-6, 470e3, 2.2e3)]
    # R408, R415, R417 onto the node, loaded by R423 + R418.
    w = np.array([1 / 12, 1 / 12, 1 / 3.3])
    w = w / (w.sum() + 1 / 12.68)

    def model(p, n, sr):
        exc_tau, clamp, asym, gain, s1, s2, s3, q1, q2, q3 = p
        t = np.arange(n) / sr
        e = np.exp(-t / (exc_tau * 1e-3))
        node = 0
        for (c, big, small), fs, qs, wi in zip(nets, (s1, s2, s3), (q1, q2, q3), w):
            w0 = fs / (c * np.sqrt(big * small))
            q = qs * 0.5 * np.sqrt(big / small)
            g = big / (2 * small)
            b, a = bilinear([1, w0 / q * (1 + g), w0 ** 2], [1, w0 / q, w0 ** 2], sr)
            node = node + wi * lfilter(b, a, e)
        v = clamp * np.tanh(node / clamp)
        return -gain * np.exp(-t / 0.047) * np.where(v > 0, asym * v, v)

    p0 = [0.5, 0.4, 1.6, 1.4, 0.98, 1.04, 0.96, 0.9, 0.95, 0.8]
    lo = [0.1, 0.02, 0.2, 0.01, 0.85, 0.85, 0.85, 0.3, 0.3, 0.3]
    hi = [3, 3, 5, 20, 1.15, 1.15, 1.15, 3, 3, 3]
    print('  file                         err   exc ms clamp  asym   f1     f2     f3     Q1    Q2    Q3')
    for name in ['RimShot909-accent000.wav', 'RimShot909-accent100.wav', 'RimShot909-shot5.wav']:
        x, sr = load(os.path.join(pack, 'RimShot', name))
        n = int(0.05 * sr)
        ref = x[:n] / np.abs(x[:n]).max()
        r = least_squares(lambda p: model(p, n, sr) - ref, p0, bounds=(lo, hi), max_nfev=3000)
        err = np.sqrt(np.mean(r.fun ** 2)) / np.sqrt(np.mean(ref ** 2))
        p = r.x
        f = [fs / (c * np.sqrt(big * small)) / (2 * np.pi) for (c, big, small), fs in zip(nets, p[4:7])]
        qn = [qs * 0.5 * np.sqrt(big / small) for (c, big, small), qs in zip(nets, p[7:10])]
        print('  %-28s %.3f  %.2f   %.2f  %.2f  %6.1f %6.1f %6.1f  %.2f  %.2f  %.2f' % (
            name, err, p[0], p[1], p[2], f[0], f[1], f[2], qn[0], qn[1], qn[2]))


# ------------------------------------------------------------ the other voices

def lowpass(x, sr, hz):
    """Zero-phase low-pass by FFT with a raised-cosine skirt an octave wide, so
    it does not ring at the hit the way a brick wall would."""
    n = 1 << int(np.ceil(np.log2(len(x) + 1)))
    spec = np.fft.rfft(x, n)
    f = np.fft.rfftfreq(n, 1 / sr)
    w = np.clip((np.log2(2 * hz / np.maximum(f, 1e-9))), 0, 1)
    spec *= 0.5 - 0.5 * np.cos(np.pi * w)
    return np.fft.irfft(spec, n)[:len(x)]


def periods(x, sr, seconds=0.5):
    """Instantaneous frequency from full periods, both polarities."""
    y = lowpass(x[:int(seconds * sr)], sr, 600)
    out = []
    for s in (1, -1):
        z = np.where((s * y[:-1] < 0) & (s * y[1:] >= 0))[0]
        tz = (z + y[z] / (y[z] - y[z + 1])) / sr
        out += [((a + b) / 2, 1 / (b - a)) for a, b in zip(tz, tz[1:])]
    return np.array(sorted(out))


def fit_sweep(tr, t0=0.003):
    """f = f_inf (1 + D e^(-t/tau)): a grid on tau, least squares for the rest."""
    tr = tr[tr[:, 0] > t0]
    t, f = tr[:, 0], tr[:, 1]
    best = None
    for tau in np.exp(np.linspace(np.log(0.003), np.log(0.1), 300)):
        basis = np.stack([np.ones_like(t), np.exp(-t / tau)], 1)
        c = np.linalg.lstsq(basis, f, rcond=None)[0]
        e = np.sum(((basis @ c) - f) ** 2 / f ** 2)
        if best is None or e < best[0]:
            best = (e, c[0], c[1] / c[0], tau)
    return best[1:]


def rms_env(x, sr, step):
    w = int(step * sr)
    n = len(x) // w
    return np.sqrt(np.mean(x[:n * w].reshape(n, w) ** 2, 1))


def slope_tau(t, db):
    return -8.686 / np.polyfit(t, db, 1)[0]


def cmd_bd(pack):
    print('  Tune   f_inf   D     tau ms')
    for tu in ['000', '025', '050', '075', '100']:
        x, sr = load(os.path.join(pack, 'BassDrum', 'BassDrum909-tune%s-attack000-decay100.wav' % tu))
        fi, d, tau = fit_sweep(periods(x, sr))
        print('  %s   %5.1f  %4.2f  %5.1f' % (tu, fi, d, tau * 1000))
    print('  Decay  hold ms  fall tau ms, as (1 + t/tau) e^(-t/tau) after the hold; tau with the hold at 45 ms')
    for d in ['000', '025', '050', '075', '100']:
        x, sr = load(os.path.join(pack, 'BassDrum', 'BassDrum909-tune050-attack000-decay%s.wav' % d))
        e = 20 * np.log10(rms_env(lowpass(x, sr, 600), sr, 0.01) + 1e-12)
        e -= np.median(e[1:4])
        t = np.arange(len(e)) * 0.01 + 0.005
        last = int(np.argmax(e < -30)) or len(e)
        t, e = t[1:last], e[1:last]
        best, fixed = None, None
        for tau in np.exp(np.linspace(np.log(0.004), np.log(0.15), 120)):
            u = np.maximum(t - 0.045, 0) / tau
            err = np.mean((20 * np.log10((1 + u) * np.exp(-u)) - e) ** 2)
            if fixed is None or err < fixed[0]:
                fixed = (err, tau)
        for hold in np.arange(0.030, 0.060, 0.0025):
            for tau in np.exp(np.linspace(np.log(0.004), np.log(0.15), 120)):
                u = np.maximum(t - hold, 0) / tau
                err = np.mean((20 * np.log10((1 + u) * np.exp(-u)) - e) ** 2)
                if best is None or err < best[0]:
                    best = (err, hold, tau)
        print('  %s    %4.0f     %5.1f    %5.1f' % (d, best[1] * 1000, best[2] * 1000, fixed[1] * 1000))


def peak_in(x, sr, a, b, lo, hi):
    s = x[int(a * sr):int(b * sr)]
    n = 1 << 18
    spec = np.abs(np.fft.rfft(s * np.hanning(len(s)), n))
    f = np.fft.rfftfreq(n, 1 / sr)
    m = (f > lo) & (f < hi)
    i = int(np.argmax(spec[m]))
    return f[m][i], 20 * np.log10(spec[m][i])


def highpass(x, sr, hz):
    spec = np.fft.rfft(x)
    spec[np.fft.rfftfreq(len(x), 1 / sr) < hz] = 0
    return np.fft.irfft(spec, len(x))


def cmd_sd(pack):
    print('  Tune   f1      f2      ratio  f2 level')
    for tu in ['000', '025', '050', '075', '100']:
        x, sr = load(os.path.join(pack, 'SnareDrum', 'SnareDrum909-tune%s-tone050-snappy000.wav' % tu))
        f1, l1 = peak_in(x, sr, 0.05, 0.2, 100, 400)
        f2, l2 = peak_in(x, sr, 0.05, 0.2, f1 * 1.3, f1 * 1.7)
        print('  %s   %5.1f   %5.1f   %.3f  %.1f dB' % (tu, f1, f2, f2 / f1, l2 - l1))
    print('  Tone   noise tau (-4..-25 dB above 2 kHz)')
    for to in ['000', '025', '050', '075', '100']:
        x, sr = load(os.path.join(pack, 'SnareDrum', 'SnareDrum909-tune050-tone%s-snappy100.wav' % to))
        e = 20 * np.log10(rms_env(highpass(x[:int(0.5 * sr)], sr, 2000), sr, 0.005) + 1e-12)
        e -= e[:6].max()
        t = np.arange(len(e)) * 0.005
        i1, i2 = int(np.argmax(e < -4)), int(np.argmax(e < -25))
        print('  %s    %.0f ms' % (to, slope_tau(t[i1:i2], e[i1:i2]) * 1000))


def cmd_clap(pack):
    import glob
    ons = []
    for f in sorted(glob.glob(os.path.join(pack, 'HandClap', 'HandClap909-*.wav'))):
        x, sr = load(f)
        e = rms_env(x[:int(0.05 * sr)], sr, 0.0002)
        rise = [i for i in range(10, len(e) - 1) if e[i + 1] > 2 * e[i - 10:i].mean() and e[i + 1] > 0.25 * e.max()]
        o = [0.0]
        for i in rise:
            if i * 0.0002 - o[-1] > 0.005:
                o.append(i * 0.0002)
        ons.append(o[:4])
    ons = np.array([o for o in ons if len(o) == 4])
    print('  burst onsets, median of %d recordings: %s ms' % (len(ons), ' '.join('%.1f' % v for v in np.median(ons, 0) * 1000)))
    x, sr = load(os.path.join(pack, 'HandClap', 'HandClap909-accent000.wav'))
    e = 20 * np.log10(rms_env(x, sr, 0.01) + 1e-12)
    t = np.arange(len(e)) * 0.01
    ok = (t > 0.06) & (t < 0.3)
    print('  tail tau %.0f ms' % (slope_tau(t[ok], e[ok]) * 1000))


def zoh_null(x, sr, lo=24000, hi=44000, t1=0.3):
    s = x[int(0.01 * sr):int(t1 * sr)]
    n = 1 << 17
    spec = np.abs(np.fft.rfft(s * np.hanning(len(s)), n)) ** 2
    f = np.fft.rfftfreq(n, 1 / sr)
    edges = np.arange(lo, hi, 250)
    lv = [spec[(f >= a) & (f < a + 500)].mean() for a in edges]
    return edges[int(np.argmin(lv))] + 250


def cmd_hats(pack):
    print('  CH Decay   tau (linear fit in dB, first 80 ms)')
    for d in ['000', '025', '050', '075', '100']:
        x, sr = load(os.path.join(pack, 'ClosedHat', 'ClosedHat909-decay%s.wav' % d))
        e = 20 * np.log10(rms_env(x[:int(0.2 * sr)], sr, 0.005) + 1e-12)
        e -= e[0]
        t = np.arange(len(e)) * 0.005 + 0.0025
        ok = (t > 0.003) & (t < 0.08) & (e > -45)
        print('  %s        %.1f ms' % (d, slope_tau(t[ok], e[ok]) * 1000))
    x, sr = load(os.path.join(pack, 'OpenHat', 'OpenHat-accent000-decay100.wav'))
    e = 20 * np.log10(rms_env(x, sr, 0.002) + 1e-12)
    t = np.arange(len(e)) * 0.002
    end = t[int(np.argmax((np.diff(e) < -10) & (t[:-1] > 0.1))) + 1]
    e10 = 20 * np.log10(rms_env(x, sr, 0.01) + 1e-12)
    t10 = np.arange(len(e10)) * 0.01 + 0.005
    ok = (t10 > 0.005) & (t10 < 0.45)
    null = zoh_null(x, sr)
    print('  OH at 100 %%: tau %.0f ms, run ends at %.0f ms; converter null %.1f kHz -> %.0f samples'
          % (slope_tau(t10[ok], e10[ok]) * 1000, end * 1000, null / 1000, end * null))


def cmd_cymbals(pack):
    for v, name in (('Crash', 'Crash909-tune%s.wav'), ('Ride', 'Ride909-tune%s.wav')):
        nulls = []
        for tu in ['000', '050', '100']:
            x, sr = load(os.path.join(pack, v, name % tu))
            nulls.append(zoh_null(x, sr, 20000, 47000, 0.5) / 1000)
        print('  %-5s converter null at Tune 0, 50, 100 %%: %s kHz' % (v, ', '.join('%.1f' % n for n in nulls)))


def cmd_levels(pack):
    grid = [('BD', 'BassDrum/BassDrum909-tune050-attack050-decay050.wav',
             'BassDrum/BassDrum909-accent%03d-tune100-attack100-decay100.wav'),
            ('SD', 'SnareDrum/SnareDrum909-tune050-tone050-snappy050.wav',
             'SnareDrum/Snare909-accent%03d-tune050-tone100-snappy100.wav'),
            ('LT', 'LowTom/LowTom909-tune050-decay050.wav', 'LowTom/LowTom-accent%03d-tune050-decay100.wav'),
            ('MT', 'MidTom/MidTom909-tune050-decay050.wav', 'MidTom/MidTom-accent%03d-tune050-decay100.wav'),
            ('HT', 'HiTom/HiTom909-tune050-decay050.wav', 'HiTom/HiTom-accent%03d-tune050-decay100.wav'),
            ('RS', 'RimShot/RimShot909-accent000.wav', 'RimShot/RimShot909-accent%03d.wav'),
            ('CP', 'HandClap/HandClap909-accent000.wav', 'HandClap/HandClap909-accent%03d.wav'),
            ('CH', 'ClosedHat/ClosedHat909-decay050.wav', 'ClosedHat/ClosedHat-accent%03d-decay100.wav'),
            ('OH', 'OpenHat/OpenHat-accent000-decay100.wav', 'OpenHat/OpenHat-accent%03d-decay100.wav'),
            ('CR', 'Crash/Crash909-tune050.wav', 'Crash/Crash-accent%03d-tune050.wav'),
            ('RD', 'Ride/Ride909-tune050.wav', 'Ride/Ride909-accent%03d-tune050.wav')]

    def rms(path, t):
        x, sr = load(os.path.join(pack, path))
        return np.sqrt(np.mean(x[:int(t * sr)] ** 2))
    ref = rms(grid[0][1], 0.15)
    print('  voice  RMS against the kick   accent span')
    for name, centre, acc in grid:
        span = rms(acc % 100, 0.3) / rms(acc % 0, 0.3)
        print('  %-5s  %6.1f dB              x%.2f' % (name, 20 * np.log10(rms(centre, 0.15) / ref), span))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('what', choices=['toms', 'rim', 'bd', 'sd', 'clap', 'hats', 'cymbals', 'levels', 'render'])
    ap.add_argument('file', nargs='?')
    ap.add_argument('--pack', default=PACK)
    a = ap.parse_args()
    if a.what == 'toms':
        cmd_toms(a.pack)
    elif a.what == 'rim':
        cmd_rim(a.pack)
    elif a.what == 'render':
        cmd_render(a.file)
    else:
        globals()['cmd_' + a.what](a.pack)


if __name__ == '__main__':
    main()
