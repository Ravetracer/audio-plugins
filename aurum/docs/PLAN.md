# Aurum Reverb – Implementation Plan

Native algorithmic reverb for Linux and Windows (CLAP first, VST3 via
clap-wrapper). Original
design based on public reverb literature (Schroeder, Moorer, Jot, Dattorro).
No third-party code, assets or data are used.

## Targets

- Formats: CLAP (native), VST3 (clap-wrapper). Linux x86-64 (X11) and
  Windows x86-64 (win32, cross-built with mingw-w64).
- Channels: mono and stereo.
- GUI: own widget toolkit on Cairo, with an X11 and a win32 window backend
  (`gui/X11Window`, `gui/Win32Window`, selected by `gui/NativeWindow.h`),
  HiDPI aware, resizable.

## Signal flow

```
in ─ input level/pan ─┬──────────────────────────────────────────────── dry ─┐
                      │                                                      │
                      ├─ duck/gate detectors                                 │
                      └─ predelay ─ thickness (ADAA saturation) ─┬─ ER ──┐   │
                                                                 │       │   │
                                                       late input diffusion  │
                                                                 │       │   │
                                       tank A / tank B (Natural FDN │ Classic │ Plate)
                                       per-segment GEQ attenuation = Decay Contour
                                                                 │       │   │
                                       width matrix ─ chorus (character) ─ sum
                                                                 │           │
                                     Tone EQ (6 bands, M/S/L/R) ─ auto gain   │
                                                                 │           │
                                             brightness tilt ─ duck ─ gate   │
                                                                 └── mix ────┤
                                                                      output level/pan ─ soft bypass ─ out
```

## Core DSP decisions

- Decay control: every recirculating segment carries a 1/3-octave graphic-EQ
  attenuation filter (cascade of peaking biquads + broadband gain). Target per
  band is `-60 dB * d / (fs * T60(f))`; band gains come from a least-squares
  solve with precomputed interaction-matrix pseudo-inverses (Välimäki/Liski
  style accurate cascade GEQ, Prawda/Schlecht FDN T60 control).
  `T60(f) = spaceT60 * decayRate * roomCurve(f) * brightness(f) * decayEQ(f)`.
- Natural: two 16-line FDNs (tank per input side), orthogonal feedback matrix
  built from four 2x2 rotations (theta = pi/4 gives Hadamard; smaller theta
  leaves discrete late echoes for Motion), allpass-interpolated modulated
  reads (lossless magnitude in the loop).
- Classic: allpass/delay ring tank in the style of early digital units, band
  limited, random "wander" modulation.
- Plate: Dattorro figure-8 tank, size scaled by Room.
- Early reflections: seeded, room-dependent multitap pattern, distance-dependent
  level, brightness and diffusion.
- Width: tank-A/tank-B cross-feed matrix (0 % mono, 50 % full cross-feed,
  100 % multi-mono, > 100 % side boost).
- Tone EQ: TPT state-variable filters (bell, shelves, Butterworth cuts up to
  96 dB/oct), per-band stereo placement, energy-based auto gain compensation.
- Freeze: input muted, loop attenuation bypassed.

## Status

Phases 1-8 implemented and tested (see README "Tests"). Windows builds since
0.2.0: tests, offscreen render and the embedded window (mouse, keyboard, file
dialog, IR import) verified under wine; no Windows DAW has loaded it yet, and
drag-and-drop of an IR onto the window is untested there. Since 0.3.0 the
sound is calibrated against reference renders (see CLAUDE.md, Aurum), and
since 0.3.1 Brightness per style and Plate's Thickness, Distance and
Decay Rate.
Phase 8 closed by ear test (2026-10-08): the sound is approved and the
factory presets are close to the reference. Remaining preset differences
are accepted -- the reference applies modulation that Aurum deliberately
does not replicate, and Aurum keeps its own character rather than aiming
for a clone. No further preset fitting is planned.
Open: VST3 build not yet verified in a host; surround support (see TODO).

## Phases

1. Scaffold: CMake, CLAP skeleton (params, state, audio ports, tail), VST3
   wrapper, offline test harness. clap-validator clean.
2. Natural engine + GEQ attenuation + ER + all main controls. Tests: T60 per
   band vs target, stability sweeps, NaN/denormal checks, CPU benchmark.
3. Tone EQ, ducking, auto gate, freeze, I/O section, soft bypass, Classic and
   Plate engines.
4. GUI toolkit: X11 window embedding, Cairo rendering, timer/fd integration,
   scaling, knobs/buttons/menus/text entry.
5. Editor panels: Room ruler, Decay Contour (third-octave decay times) and
   Tone EQ with band inspector; no analyser.
6. Presets (browser, tags, favorites, search), undo/redo, A/B, MIDI learn.
7. IR import (WAV/AIFF, Schroeder EDC per band, fitting of Room, Decay
   Contour and Tone EQ).
8. Factory presets, sound tuning, polish.

Phase 6 also gets an importer for external `.ffp` text presets (user-supplied files; never bundled in this repo).

## TODO

- **Surround.** Aurum is mono/stereo only. Hosts other than the one used
  for development offer multichannel tracks, and a multichannel reverb is
  useful for film, game and immersive mixes. To work out:
  - Layouts: 5.1 and 7.1 first, 7.1.4 as a stretch; mono/stereo in to
    surround out as well as surround in to surround out.
  - CLAP: `audio-ports-config` (or `configurable-audio-ports`) with
    `surround` channel maps; VST3: speaker arrangements through
    clap-wrapper, which must be verified to pass them through.
  - Engine: decorrelated outputs per channel from the FDN (more output
    taps, not copies), early reflections per speaker position, a centre
    channel that does not collapse the image, LFE left dry or band-limited.
  - Controls: Width generalised to a front/rear and height spread; the
    `.ffp` importer's surround keys, ignored today, mapped where they fit.
  - Testing: no multichannel host is available here; a multichannel path
    in the offline test harness and `clap_gui_host` is needed first.
