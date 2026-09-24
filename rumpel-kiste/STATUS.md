# RumpelKiste -- status

Version 0.1.0. Linux and Windows, CLAP and VST3. 76 parameters, 19 presets.
Builds clean on both platforms; `render --selftest` passes all 148 checks on
the Linux build and on the Windows build under wine.

## What works, and what is measured

The self-test holds the voices to the numbers the service notes give, at 44.1,
48 and 96 kHz:

- the kick settles on BD Pitch and its first cycle is more than 1.7 times
  higher; Tune lengthens the sweep without moving the landing note; Decay and
  Attack do what the circuit says
- the snare's oscillators sit at SD Pitch and 1.47 times it (C69/C71), Tune
  spans an octave, Snappy brings in the noise, Tone lengthens the tail
- the toms sit at 1 : 1.222 : 1.5 (C19 : C33 : C102), Tune spans an octave,
  and they bend down from a sharper start
- the rim shot's energy is above its 495 Hz high-pass and dies within 50 ms
- the clap has four bursts
- the closed hat is shorter than the open one and chokes it; the hats are
  high-pass metal; six bits are audible and their noise decays with the sound
- a crash tuned up is shorter, like a faster ROM clock; the ride pings over a
  wash
- every voice is finite, in range, and louder accented than plain
- the drive bus: Master distorts the mix, Selected drives only routed voices
  and leaves the rest bit-identical

and the sequencer and the plugin around it:

- hits land on their frame to the sample; Shuffle, Scale and Last Step place
  steps where they should; a chain moves on and wraps; a flam's second stroke
  arrives Flam × Flam Unit later; a looping host drops no step at the seam
- MIDI keys play their voices, unmapped keys play nothing
- state round-trips byte for byte; the same events render bit-identical
  samples; the dragged MIDI file carries every hit
- every factory preset loads, makes a sound and stays under full scale

The window was driven on Xvfb and photographed on both backends: the X11 one
natively and the win32 one under wine. Two instances in one process show two
different banks, and `nm -C --defined-only RumpelKiste.clap | grep " [bB] "`
shows nothing per-instance at file scope.

## What has not been done

- **No DAW has loaded it.** wine is not Windows and guihost is not a host.
- **It has not been compared against a real machine by ear**, or against
  recordings of one. Everything above is the build agreeing with the schematic.
- The hi-hat and cymbal content is a synthesised stand-in (see the manual,
  §5). How close it gets to the ROMs has not been judged against anything.

## Bugs found and fixed on the way

- Every row label read as permanently lit on Windows: the flash timer kept a
  steady clock in milliseconds in a `long`, which is 32 bits there and wraps
  after 24.8 days of uptime. Found by photographing the win32 window under
  wine; now `int64_t`.
- `near()` in the renderer collided with `<windows.h>`'s macro of the same name.
