# Shapeau -- plan

The source of truth for what Shapeau is meant to become and in what order.
`STATUS.md` says what is done and measured; this says where it is going.

## What it is meant to be

A rack of effects, each driven by a shape drawn against the beat, every one of
them able to run differently in three frequency bands. The editor is the
instrument: a large shape editor that shows the audio passing through it, so a
duck can be drawn onto the kick it is ducking for, and a chain of effects that
is built rather than fixed -- added to, reordered, removed from.

The model is a commercial shape-based effects rack the user works with and
whose behaviour Shapeau is measured against. **It is never named anywhere in
this repository** -- code, comments, documentation, presets, commit messages --
because Shapeau is meant for a free public release. Shapeau's names are its
own.

## Phases

Each phase ends with a build the user tests before the next one starts.

### Phase 1 -- the scope and the chain (0.3.0)

- **The scope behind the shape.** The signal going in (grey) and coming out (in
  the accent), drawn along the selected effect's cycle so the waveform sits
  under the part of the shape that acts on it; a playhead; and the modulation
  trace -- what the effect actually does after Smooth, which leaves the drawn
  shape wherever smoothing rounds it off. Sample accurate: the audio thread
  writes the bins itself, along the same phase the shape is read at.
- **One effect at a time.** The effect bar above the editor shows the chain in
  order; the editor and the selected effect's panel are what is on screen, with
  the global controls beside them. An empty chain shows what can be added.
- **A chain that is built.** Add (+), remove, drag to reorder, and a power dot
  that bypasses an effect without taking it out. The order is the order the
  audio runs in, so a filtered stutter and a stuttered filter are two
  different things.
- The Smooth controls of Filter and Drive, which were read and never applied.

### Phase 2 -- editing

Selection box with move, scale and skew; line, arc and S-curve pens; undo and
redo per effect and band; flip, double, nudge left/right, randomise; copy and
paste; a triplet grid; wave presets per effect with a favourites shelf.

### Triggers and the sidechain (0.4.0, pulled forward from phase 3)

A sidechain input, drawn behind the shape; per effect a trigger (the beat, a
MIDI note or a hit in the sidechain or the input), loop or one-shot, and an
End marker dragged in the editor; one shared detector setup. Done -- see
`STATUS.md`.

### Phase 3 -- timing and triggers

Per-band rate (linked by default); a free rate in Hz; an envelope follower per
effect, with sidechain; per-effect mix and band solo. Possibly a lookahead for
the audio trigger (latency in exchange for a duck that starts before the hit).
The MIDI and audio triggers, the one-shot with its end marker and the
sidechain input were done in 0.4.0.

### Phase 4 -- more effects

Noise, a phaser/flanger/chorus effect, a reverb, a pitch shifter, a compressor
and an oscilloscope tool. Each one is an effect in the chain like the rest:
a panel, a shape per band, a place in the order.
