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
| BD pitch envelope, main | C9 0.33 µF, R57 22 k + VR2 100 k (A) | 7.3–40 ms |
| BD pitch envelope, fast | C1 0.068 µF, R12 100 k | 6.8 ms |
| BD amp attack | C8 0.33 µF, R35 1.8 k | 0.59 ms |
| BD amp decay | C8 0.33 µF, R58 47 k + VR5 1 M (A) | 15.5–345 ms |
| BD click envelope | C12 0.033 µF, R41 22 k | 0.73 ms |
| BD click pulse | C5 0.0068 µF, R14 22 k | 0.15 ms |
| BD click noise low-pass | R45 4.7 k, C13 0.1 µF | 339 Hz |
| BD pulse coupling | C11 0.47 µF, R8 ∥ R44 | 49 Hz |
| SD oscillator ratio | C69 0.01 µF / C71 0.0068 µF | 1.4706 |
| SD pitch bend | IC36 supply 5 V → 2 V (p.6 figure), C66 0.01 µF, R248 470 k | ×2.5 at the hit, 4.7 ms |
| SD VCO1 envelope | C83 0.1 µF, R291 220 k | 22 ms |
| SD VCO2 envelope | C82 0.047 µF, R295 220 k | 10.3 ms |
| SD noise tail (Tone) | C67 0.47 µF, R254 100 k + VR7 500 k (B) | 47–282 ms |
| SD snap burst | trigger width 2 ms (p.5), C73 470 pF, R280 220 k | 2 ms + 0.1 ms |
| SD noise low-pass | IC40a: R300 = R301 22 k, C81 3300 pF, C84 220 pF | 8.49 kHz, Q 1.94 |
| SD snap high-pass | IC39a: C75 = C76 2200 pF, R277 47 k, R276 6.8 k | 4.05 kHz, Q 1.31 |
| SD, tom Tune range | VR 10 k between 100 k/10 k (SD) or 68 k/10 k (toms) | 2:1, one octave |
| Tom oscillator ratios | LT C19/C18/C20 0.033/0.022/0.012 µF | 1, 1.5, 2.75 |
| | MT C33/C32/C34 0.027/0.018/0.01 µF | 1, 1.5, 2.7 |
| | HT C102/C97/C103 0.022/0.015/0.0082 µF | 1, 1.467, 2.683 |
| Toms against each other | C19 : C33 : C102 | 1 : 1.222 : 1.5 |
| Tom pitch envelopes | C17 0.047 µF, R61 470 k / C16 0.1 µF, R60 2.2 M | 22 ms / 220 ms |
| Tom main decay | C23 0.68 µF, R111 56 k + VR11 500 k (B) | 38–378 ms |
| Tom top oscillator envelope | C25 0.056 µF, R110 470 k | 26 ms |
| Tom noise high-pass | C51 = C52 4700 pF, R191 10 k, R192 220 k | 722 Hz, Q 2.35 |
| Tom noise gate | C54 0.0047 µF (after the change note), R198 100 k | 0.47 ms |
| RS excitation | C111 0.018 µF, R395 ∥ R396 22 k | 0.2 ms |
| RS resonator F1 | C112/C113 0.01 µF, R407 470 k, R394 2.2 k | 494.9 Hz, Q 7.31 |
| RS resonator F2 | C115/C116 0.027 µF, R414 330 k, R411 2.2 k | 218.8 Hz, Q 6.12 |
| RS resonator F3 | C117/C118 0.0047 µF, R416 470 k, R404 2.2 k | 1053 Hz, Q 7.31 |
| RS mix | R408 12 k, R415 12 k, R417 3.3 k | 1 : 1 : 3.64 |
| RS high-pass | IC50b: C121 = C122 0.01 µF, R420 220 k, R419 4.7 k | 495 Hz, Q 3.42 |
| CP band-pass | IC26b MFB: R208 47 k, R209 10 k, R207 150 k, C42 = C43 4700 pF | 963 Hz, Q 2.13, gain 1.6 |
| CP claps high-pass | C56 0.022 µF, R202 39 k | 185 Hz |
| CP tail low-pass | R204 1 k, C57 0.47 µF | 339 Hz |
| CP tail decay | C59 0.1 µF, R233 470 k | 47 ms |
| CP burst count | p.9 printed waveform | 4 |
| Noise clock | IC31 XOR oscillator, R185 33 k, C47 100 pF, f ≈ 1/(2.2RC) | 138 kHz |
| Noise register | two 4006 (p.6: 32 stages) | 32-bit LFSR |
| HH ROM clock | p.5: "about 60 kHz, divided by two" | 30 kHz |
| HH ROM runs | p.5 address table | OH 24576, CH 8192 samples |
| HH decay ratio | p.5: CH path is 1/10 of R452 + VR23 | CH range = OH range / 10 |
| DAC resolution | IC68 latch, six data lines to RA9 | 6 bits |
| Trigger width | p.5 | 2 ms |
| Scale, shuffle, flam, last step | owner's manual pp. 24–26 | 4 scales, 7 shuffles, 8 flams, 1–16 steps |
| Flam order | owner's manual p.26 | light stroke on the step, full one after |
| Local accent voices | owner's manual p.25 | BD SD LT MT HT CH (all here) |
| MIDI keys | service notes p.15 | 35/36 BD … 51 RD |

## Fitted: controls, not constants

| Mod | Default | Why it is fitted |
|-----|---------|------------------|
| BD Pitch | 50 Hz | R27 (1.5 M) is given; the frequency it makes is not |
| BD Sweep | 3.0× | how high the envelopes drive the oscillator depends on unprinted levels |
| BD Shape | 50 % | the triangle's swing against D10/D11's knee |
| SD Pitch | 190 Hz | as BD Pitch |
| Tom Pitch | 90 Hz | as BD Pitch |
| Tom Sweep | 50 % | the envelope capacitors' starting charge |
| Tom Noise | 50 % | the noise tick's level against the oscillators |
| Rim Gate | 5 ms | Q64/Q65's timing is not legible |
| Clap Spread | 9 ms | from the p.9 waveform; the timing oscillator is not legible |
| Hat Color, Cym Color | 50 % | the ROM content is not reproduced |
| Local Accent | 60 % | the accent resistor arrays' values are not printed |
| Shuffle Unit | 1/12 step | the machine's shuffle step is not given |
| Flam Unit | 4 ms | the flam intervals' lengths are not given |
| Flam Grace | 60 % | the first stroke's level is not given |

Also fitted, and not exposed: the voices' relative output gains in
`drums.cpp` (`kBdGain` and the rest), balanced by rendering; C135's value
(illegible), fitted to the p.9 hat traces; the cymbal ROM envelopes' depth in
dB, fitted to the same page; the clap tail's rise time.
