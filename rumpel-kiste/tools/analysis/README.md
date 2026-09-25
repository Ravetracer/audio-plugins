# Where the numbers come from

Every constant in `src/dsp/drums.cpp` and `src/params.cpp`, and what it was
read from. The source is the TR-909 service notes (Roland, June 15 1984, First
Edition), in `!dev/TR-909.pdf`: circuit descriptions on pages 5 and 6, printed
waveforms on page 9, the voicing board's circuit diagram on page 11, the MIDI
implementation on page 15. The owner's manual (`!dev/TR-909_OM.pdf`) gives the
sequencer's behaviour.

Formulas: an RC time constant is R·C. A unity-gain Sallen-Key low-pass with
equal R has f = 1/(2πR√(C1C2)) and Q = ½√(C1/C2). A Sallen-Key high-pass with
equal C has f = 1/(2πC√(R1R2)) and Q = ½√(R_ground/R_feedback). A bridged-T in
an op-amp's feedback has f = 1/(2πC√(Rr)) and Q = ½√(R/r).

## Fixed: computed from the diagram

| What | Parts | Value |
|------|-------|-------|
| BD pitch envelope, main | C9 0.33 µF, R57 22 k + VR2 100 k (A) | 7.3–40 ms (measured: 7.6–29 ms) |
| BD pitch envelope, fast | C1 0.068 µF, R12 100 k | 6.8 ms; not visible in the recordings, not modelled |
| BD amp attack | C8 0.33 µF, R35 1.8 k | 0.59 ms |
| BD amp decay | C8 0.33 µF, R58 47 k + VR5 1 M (A) | 15.5–345 ms, what the knob shows; the audible hold and fall are measured |
| BD click envelope | C12 0.033 µF, R41 22 k | 0.73 ms |
| BD click pulse | C5 0.0068 µF, R14 22 k | 0.15 ms |
| BD click noise low-pass | R45 4.7 k, C13 0.1 µF | 339 Hz |
| BD pulse coupling | C11 0.47 µF, R8 ∥ R44 | 49 Hz |
| SD oscillator ratio | C69 0.01 µF / C71 0.0068 µF | 1.4706 (measured: 1.500, used) |
| SD pitch bend | IC36 supply 5 V → 2 V (p.6 figure), C66 0.01 µF, R248 470 k | ×2.5 at the hit, 4.7 ms |
| SD VCO1 envelope | C83 0.1 µF, R291 220 k | 22 ms (measured: 27 ms) |
| SD VCO2 envelope | C82 0.047 µF, R295 220 k | 10.3 ms (measured: 25 ms) |
| SD noise tail (Tone) | C67 0.47 µF, R254 100 k + VR7 500 k (B) | 47–282 ms (measured: 24–77 ms) |
| SD snap burst | trigger width 2 ms (p.5), C73 470 pF, R280 220 k | 2 ms + 0.1 ms |
| SD noise low-pass | IC40a: R300 = R301 22 k, C81 3300 pF, C84 220 pF | 8.49 kHz, Q 1.94 |
| SD snap high-pass | IC39a: C75 = C76 2200 pF, R277 47 k, R276 6.8 k | 4.05 kHz, Q 1.31 |
| SD, tom Tune range | VR 10 k between 100 k/10 k (SD) or 68 k/10 k (toms) | 2:1, one octave |
| Tom oscillator ratios | LT C19/C18/C20 0.033/0.022/0.012 µF | 1, 1.5, 2.75 |
| | MT C33/C32/C34 0.027/0.018/0.01 µF | 1, 1.5, 2.7 |
| | HT C102/C97/C103 0.022/0.015/0.0082 µF | 1, 1.467, 2.683 |
| Toms against each other | C19 : C33 : C102 | 1 : 1.222 : 1.5 |
| Tom pitch envelopes | C17 0.047 µF, R61 470 k / C16 0.1 µF, R60 2.2 M | 22 ms (measured: 30 ms) / 220 ms |
| Tom main decay | C23 0.68 µF, R111 56 k + VR11 500 k (B) | 38–378 ms, what the knob shows; the audible decay is measured |
| Tom top oscillator envelope | C25 0.056 µF, R110 470 k | 26 ms (measured: 26–29 ms) |
| Tom noise high-pass | C51 = C52 4700 pF, R191 10 k, R192 220 k | 722 Hz, Q 2.35 |
| Tom noise tick | C54 0.0047 µF (after the change note), R198 100 k | 0.47 ms, at the end of the trigger |
| RS excitation | C111 0.018 µF into R396 + R395 (44 k), D89 clamping the negative edge | 0.79 ms (measured: 0.48 ms) |
| RS resonator gains | R407/R394, R414/R411, R416/R404: gain R / 2r on top of the input | 107, 75, 107 |
| RS resonator F1 | C112/C113 0.01 µF, R407 470 k, R394 2.2 k | 494.9 Hz, Q 7.31 |
| RS resonator F2 | C115/C116 0.027 µF, R414 330 k, R411 2.2 k | 218.8 Hz, Q 6.12 |
| RS resonator F3 | C117/C118 0.0047 µF, R416 470 k, R404 2.2 k | 1053 Hz, Q 7.31 |
| RS mix | R408 12 k, R415 12 k, R417 3.3 k | 1 : 1 : 3.64 |
| RS gate | C119 0.047 µF into R403 1 M | 47 ms, Rim Gate's default |
| RS high-pass | IC50b: C121 = C122 0.01 µF, R422 22 k from the gain-of-two divider R421/R419, R420 220 k | 229 Hz, Q 1.58; not in the recordings, not modelled (see below) |
| CP band-pass | IC26b MFB: R208 47 k, R209 10 k, R207 150 k, C42 = C43 4700 pF | 963 Hz, Q 2.13, gain 1.6 |
| CP claps high-pass | C56 0.022 µF, R202 39 k, after the VCA | 185 Hz (measured: two poles at 400 Hz) |
| CP tail low-pass | R204 1 k, C57 0.47 µF | 339 Hz |
| CP tail decay | C59 0.1 µF, R233 470 k | 47 ms (measured: 105 ms) |
| CP burst count | p.9 printed waveform | 4 |
| Noise clock | IC31 XOR oscillator, R185 33 k, C47 100 pF, f ≈ 1/(2.2RC) | 138 kHz |
| Noise register | two 4006 (p.6: 32 stages) | 32-bit LFSR |
| HH ROM clock | p.5: "about 60 kHz, divided by two" | 30 kHz (measured: 31.5 kHz) |
| HH ROM runs | p.5 address table | OH 24576, CH 8192 samples (measured: OH 16384) |
| HH decay ratio | p.5: CH path is 1/10 of R452 + VR23 | CH range = OH range / 10, what the knobs show; the audible ratio is measured |
| DAC resolution | IC68 latch, six data lines to RA9 | 6 bits |
| Trigger width | p.5 | 2 ms |
| Scale, shuffle, flam, last step | owner's manual pp. 24–26 | 4 scales, 7 shuffles, 8 flams, 1–16 steps |
| Flam order | owner's manual p.26 | light stroke on the step, full one after |
| Local accent voices | owner's manual p.25 | BD SD LT MT HT CH (all here) |
| MIDI keys | service notes p.15 | 35/36 BD … 51 RD |

## Measured: from recordings of a machine

A commercial TR-909 sample pack (Audiorealism, 96 kHz / 24 bit, recorded from
the individual outputs with every part at 50 %: the kick's and the snare's
knobs in 25 % steps, the toms' in 10 % steps, the hats' decays, the cymbals'
tunes, eleven accent levels per voice), kept in `!dev/TR-909-96kHz-24bit` and
not ours to redistribute. `measure909.py` reproduces the core of the numbers
below -- `toms`, `rim` (needs scipy), `bd`, `sd`, `clap`, `hats`, `cymbals`,
`levels`, and `render file.wav` to run the tom measurements on a render. The
rest (the stand-ins' spectra, the burst and click shapes, the ride's partials)
came from the same recordings by the methods in the last column.

These are measurements of one machine, not guesses, and they are constants in
`drums.cpp` rather than controls. Where a measured number and a component
value disagree the measurement is used and the component value is listed
above.

| What | Value | How |
|------|-------|-----|
| Which tom oscillator is the pitch | C18's (the middle one); C19's sits a fifth under it | partials at 59 / 89 Hz on the low tom, the upper one 4–6 dB louder |
| Tom sweep | +0.26 × pitch, 30 ms; +0.12 × pitch, 220 ms; in Hz, not a ratio | zero crossings of the band round the pitch, three periods at a time; the sweep is the same number of hertz across Tune |
| Tom decay | τ = τ₁ (0.4 + 0.8x − 0.2x²), τ₁ = 114 / 79 / 84 ms (LT/MT/HT), x the pot's travel | least-squares partial amplitudes in 40 ms windows, every Decay step |
| Tom VCA knee | tanh(h e^(−t/τ)) / tanh(h), h = e^(0.73 / (0.4 + 0.8x − 0.2x²)) | the same fits: the hold before the fall lasts about 0.73 τ₁ at any setting |
| C19's oscillator | 0.97 / 1.18 / 1.13 of the pitch, lagged 35 / 20 / 25 ms, 1.08 τ | its level against the pitch's at 100 ms (−6.6 / −5.3 / −4.7 dB at Decay 50 %) |
| C20's oscillator | 0.66 / 0.75 / 0.66, τ 27 / 29 / 26 ms | the same fits |
| Comparator square | 0.23 of the pitch, 13 ms | waveform fit of the low tom's first 20 ms |
| Tom noise | wash 0.05, 20 ms; tick 0.2 at 2 ms | energy above 1.5 and 3 kHz in 0.5 and 10 ms windows |
| Tom and rim accent | ×2.3 and ×1.6 top to bottom; tom noise ×3.7 | peak and RMS over the eleven accent files |
| RS resonators | 486.5 Hz Q 6.7, 228 Hz Q 5.9, 1014 Hz Q 5.7 | least-squares fit of the model to the first 50 ms of the waveform, three recordings agreeing within 1 % |
| RS clamp and asymmetry | tanh at 0.39 of the excitation; one polarity 1.58× the other | the same fit |
| BD sweep | f = BD Pitch (1 + 4.6 e^(−t/τ)), τ 7.6 / 9.7 / 11.8 / 14.5 / 28.9 ms at Tune 0–100 %, the same at every accent | zero-crossing periods, both polarities, fitted from 3 ms on |
| BD hold and fall | 45 ms flat, then (1 + t/τ) e^(−t/τ), τ 16 / 25 / 32 / 42 / 64 ms at Decay 0–100 % | 10 ms RMS of the low-passed kick, fitted in dB down to −30 dB |
| BD shape and polarity | D10/D11 offset −0.05: 2nd −28 dB, 3rd −27 dB; the first half-cycle negative | harmonic levels at 120–300 ms |
| BD click | 0.35 + 0.57 x² of Attack's travel | energy above 1 kHz in the first 4 ms against the body |
| SD | tone τ 27 / 25 ms, upper oscillator −9.7 dB; noise tail τ 24 / 42 / 57 / 75 / 77 ms at Tone 0–100 %; noise −6.8 dB against the tone; polarity negative | partial fits and energy above 2 kHz |
| CP | bursts at 0, 10.0, 21.5, 31.8 ms (the script's coarser detector reads 10.2, 22.0, 32.0); each falls 0.6 ms and 4.5 ms; band-pass Q 1.25 for the bursts, tail τ 105 ms from the last burst | onsets and burst envelopes averaged over 19 recordings; spectra |
| CH, OH | τ 8.7 / 25.5 / 33.1 / 39.3 / 44.5 ms at CH Decay 0–100 %, OH 120 ms at 100 %; CH's recording fades 0.3 dB/ms faster after 85 ms; OH starts 6.8 dB under CH | linear fits in dB; the converter's null for the clock; the run's end |
| CR, RD | clock 28.5–45.8 kHz, linear in Tune (the crash's null; the ride's at Tune 0 is masked by its own spectrum); envelope −20u − 25u⁴ dB over the run, the ride −5 dB more at the hit; ride bell partials 4.02, 4.07, 4.61, 6.73, 7.28, 7.61, 8.16, 9.93 kHz | the converter's null; envelopes; spectral peaks |
| Stand-in spectra | the filters and levels at Hat / Cym Color 50 % | a coordinate search against band levels at 0–60 ms (hats), 0–100 and 300–600 ms (cymbals): 1.3, 2.4 and 2.9 dB mean error |
| Accent | BD ×2.0, SD ×2.0 (noise ×1.4), toms ×2.3, RS ×1.6, CP ×1.8, hats ×2.3, cymbals ×2.45 | RMS over the eleven accent files |
| Output gains | every voice's RMS over its first 150 ms against the bass drum's, all parts at 50 % in both | the grid files at their centre settings |

The rim shot's IC50b reads as a 229 Hz high-pass with a 4 dB peak, and the
earlier reading of it (495 Hz, Q 3.42) was wrong: R422 feeds back from the
node between R421 and R419, not from the output. The recordings, which come
from the individual output, show no high-pass at all -- the fit with one in
is twice as far from the waveform as the fit without -- so it is left out.

## Fitted: controls, not constants

| Mod | Default | Why it is fitted |
|-----|---------|------------------|
| BD Pitch | 50 Hz | R27 (1.5 M) is given; the frequency it makes is not |
| BD Sweep | 5.6× | how high the envelope drives the oscillator depends on unprinted levels; the default is measured |
| BD Shape | 50 % | the triangle's swing against D10/D11's knee; 50 % gives the measured harmonics |
| SD Pitch | 174 Hz | as BD Pitch; the default is measured |
| Tom Pitch | 86 Hz | measured: the low tom's middle oscillator at Tune 50 %, settled |
| Tom Sweep | 50 % | the envelope capacitors' starting charge; 50 % is the measured 38 % |
| Tom Noise | 50 % | the noise's level against the oscillators; 50 % is the measured one |
| Rim Gate | 47 ms | C119 · R403; how Q65 turns that into a gate is not given |
| Clap Spread | 10.6 ms | the timing oscillator is not legible; the default and the uneven gaps are measured |
| Hat Color, Cym Color | 50 % | the ROM content is not reproduced; 50 % is fitted to the recordings' spectra |
| Local Accent | 60 % | the accent resistor arrays' values are not printed |
| Shuffle Unit | 1/12 step | the machine's shuffle step is not given |
| Flam Unit | 4 ms | the flam intervals' lengths are not given |
| Flam Grace | 60 % | the first stroke's level is not given |

Nothing else is fitted without a control: the voices' output gains, the hat
decays C135 would have set and the cymbal envelopes are all measured now (see
above).
