# Shapeau — to do

## 1. Listen to it, and tune by ear

Every number in the engine came from a measurement of what the stages should
do, not from listening. `--selftest` proves the band split is flat, the volume
curve is -12 dB at half, the time stage is transparent at the top, pan and
width are neutral in the middle, and every drive style stays bounded and
distinct. None of that says the nine presets are worth having, or that a 4-bit
crush at 8x is a good default. Things to judge first:

- the duck preset against a real sidechain — is 16 % of a cycle enough dip, and
  is the low band's 220 Hz split where a kick actually sits
- the filter's resonance at the top of its travel: Q 50 is a whistle on a
  sustained note and might be more than a "wobble" preset wants
- the drive stage's level compensation across the six styles, which is one
  curve for all of them on purpose and may not hold at Crush
- the time stage's Step default (30 %) on a stutter: too smooth is a smear,
  too hard is a click

## 2. Sample rates

Only 48 kHz has been through the self-test. The engine derives every
coefficient from the sample rate, and the renderer takes `--rate`, so 44.1,
88.2 and 96 kHz should be measured for the band split's flatness (the tolerance
in the Linkwitz-Riley split is the thing most likely to drift) and for the time
stage's read head, which is interpolated in samples.

## 3. Windows and VST3 in a DAW

Both build, and the Windows self-test passes under wine, but neither format has
been loaded by a real host on Windows.

## 4. Features

The features are planned in `docs/PLAN.md`, in four phases, and this list is
no longer where they are tracked. Phase 2 (editing tools, undo, wave presets)
is next.


## 5. Release material

- **Demo renders.** `presets/demo-descriptions.txt` is written, and
  `shared/tools/make-demos.sh shapeau` has not been run yet. It needs ffmpeg
  and lame, both in the build container.
