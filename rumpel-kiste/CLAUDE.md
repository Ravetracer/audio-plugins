# RumpelKiste

A rhythm composer: a model of the Roland TR-909's voicing board, from the
**June 15 1984 service notes**, as a native CLAP plugin, with a step sequencer
and a drive bus.

Read the repository root `CLAUDE.md` first -- the shared library, the build,
the release tooling and the conventions. Then `STATUS.md` and `TODO.md`. This
file is only what is specific to this plugin.

## Sources

`!dev/` holds the service notes (`TR-909.pdf` is the best scan; the other
service-manual PDF is the same document, worse) and the owner's manual
(`TR-909_OM.pdf`). **Not ours to redistribute**: gitignored, never committed,
never shipped. They are image-only scans; read them by rendering pages with
`pdftoppm` or extracting the 400 dpi images with `pdfimages` and cropping.
Page 11 is the voicing board and is legible at native resolution.

`tools/analysis/README.md` lists every number and the parts it came from.
Keep it in step with `drums.cpp`.

## The one rule

The same as SäureKiste's: **everything that can be read off the schematic is
fixed; everything that cannot is a control, not a hidden guess.** The mods
under ADVANCED are those controls. `TODO.md` §2 lists the constants that still
break the rule.

## What is different from SäureKiste

- **It is a drum machine, so MIDI always plays.** Notes play voices in both
  modes; in Sequencer mode a *mapped* note selects a pattern instead and is
  consumed. There is no Live mode and no transpose -- the pattern map is
  available in Sequencer mode.
- **PLAY runs the sequencer with the host stopped** (`mFreeRun`, not saved).
  A held key did that in SäureKiste; here a key plays a drum.
- **A step is a 32-bit word**, not 16: two bits per voice, a total-accent bit
  and five flam bits. See `pattern.h`.
- **A preset without a bank is a kit.** It leaves the patterns *and* the groove
  parameters (`isGrooveParam()` in `plugin.cpp`) alone, even ones the file
  names, so a saved preset with its pattern lines deleted is a kit.
- **Mutes are plugin state, not preset state**, saved in the blob after the
  chains.
- **The engine computes each voice separately** and sums them onto a dry bus
  and a drive bus. That is also what individual outputs would hang off.

## Things that are easy to get wrong

- **BD Tune is not the kick's pitch.** VR2 is in C9's discharge path: it sets
  how long the sweep lasts. The landing pitch is BD Pitch. The self-test checks
  that Tune does not move it.
- **The hi-hat is one voice.** CH and OH are two address ranges of one ROM
  through one VCA, so a closed hat chokes an open one, and the mute and drive
  route that apply are those of whichever row played last.
- **The ROM voices' decay comes after the converter.** The samples were
  stored compressed and the envelope restores them, so quantisation noise
  fades with the sound. Moving the envelope in front of the quantiser would
  sound wrong and the self-test checks the noise decays.
- **Each ROM "recording" is identical every hit** -- the stand-in's oscillators
  and noise are reset on the trigger. The analog voices share the one running
  noise register and are not, which is the machine too.
- **The rim shot clips whatever the accent**, so its accent is applied at the
  gate after the diodes (Q64/Q65), not at the excitation.
- **`drive.{h,cpp}` is SäureKiste's, copied unchanged** but for the namespace.
  Port a fix across in either direction.
- **`seqwindow.cpp` is a fork of SäureKiste's fork of the shared window.**
  Nothing ports fixes into it. It already needed one Windows-only fix of its
  own (`int64_t` milliseconds, see `STATUS.md`).
- **The parameter table is indexed by id.** New rows go at the end of both the
  enum and the table, whatever panel they belong to. The drive bus is appended
  after the mods for that reason.
- **Manual copy names no brands** except the trademark notice in chapter 1 --
  the machine is "the classic 1984 rhythm composer", pedals are "a 1970s
  stompbox" and "a 1993 dual overdrive". It uses `{{PARAMETER_SUMMARY}}`, not
  the full reference, because the tips name hardware. It is written for
  somebody with the zip and nothing else: no repository, source, renderer or
  self-test. Grep the rendered HTML.
- **`install.sh` is its own**, like SäureKiste's: `rumpel-kiste` is not a
  usable shell identifier for the shared script.
- **The manual's screenshots are committed, not built**, taken on Xvfb with
  guihost and an empty `XDG_CONFIG_HOME`. Retake them when the window moves.
