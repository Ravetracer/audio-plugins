# SidMidi -- plan

SidMidi is a Commodore 64 inside a plugin. It emulates the machine closely
enough that the original MIDI programs written for it -- Cynthcart, Station64,
SID-Wizard, SID Synth and anything like them -- run unmodified, receive the
DAW's notes through an emulated MIDI cartridge, and play the SID chips into the
DAW. Whatever the program sends back out of the cartridge comes back to the DAW
as MIDI.

It is not a SID synthesizer with a C64 skin. The instrument is the C64
program; SidMidi is the computer it runs on.

## Ground rules

- **Licence: GPL-2.0-or-later for `sidmidi/` only.** The SID chips are
  reSIDfp (libresidfp 1.2.3, Dag Lem, Antti Lankila, Leandro Nini), vendored
  unchanged in `third_party/libresidfp/` with its `COPYING`. The rest of the
  repository stays MIT; `sidmidi/LICENSE` says so.
- **Standalone, not on `shared/`**, like Aurum and Substrike: a C64 screen,
  a machine settings page and a keyboard grab do not fit `WindowSpec`. The
  window backends and widgets are copied from Substrike and belong to SidMidi
  from then on. The *Rule for a shared change* does not cover it.
- **The emulator is ours**, written for this plugin: one 6502 core for the
  C64 and the drive, VIC-II, two CIAs, the PLA, the 6850 MIDI cartridges, the
  virtual drive and the 1541. No emulator code is copied from anywhere.
- **Programs and ROMs are never committed and never shipped.** The C64
  programs in `_dev_/` are their authors'; `_dev_/` is gitignored. The
  Commodore ROMs are never compiled in: the user puts them into SidMidi's ROM
  folder (`~/.local/share/SidMidi/ROMs`, `%APPDATA%\SidMidi\ROMs`), which the
  plugin creates with a README and explains in its window. Files are
  recognised by content. The ROMs in `_dev_/roms` are the developer's, for
  the tests only (`SIDMIDI_ROM_DIR`).
- **Everything per-instance lives in the instance.** Two SidMidi instances
  are two C64s. reSIDfp's filter tables are read-only singletons, which is
  fine; its 6581 *filter range* and *old caps* settings mutate a singleton
  and are therefore not exposed until they are made per-chip (phase 4).
- **Deterministic.** The machine runs in the audio thread, clocked by the
  sample count. The same project renders the same audio, online or offline.

## How it fits together

```
 DAW notes/CC ──► MIDI bytes ──► 6850 ACIA ($DE00 page) ──► C64 program (IRQ/NMI)
 DAW transport ─► MIDI clock (optional) ─┘                       │
                                                                 ├──► SID 1..8 ──► audio out
 GUI keyboard/joystick/mouse ──► CIA matrix, SID POT ────────────┤
 .prg/.d64/.crt ──► memory / virtual drive / cartridge port ─────┘
 ACIA transmitter ──► MIDI bytes ──► DAW note out
 VIC-II ──► frame buffer (palette indices) ──► GUI screen
```

### Timing

- PAL 985248 Hz (6569, 63 cycles x 312 lines), NTSC 1022727 Hz (6567R8,
  65 x 263). One host block runs `blockSamples * clock / sampleRate` cycles,
  with the fraction carried.
- Host events are converted to the cycle they belong to; the machine runs up
  to that cycle, then the event is applied. MIDI bytes go into the ACIA's
  receive line, which delivers one byte per 10-bit frame at 31250 baud
  (~315 cycles PAL), so a held-up program overruns like the real thing.
- Every CPU bus access is one cycle and ticks the VIC, the CIAs and the ACIA.
  The CPU stalls on reads while the VIC holds BA low (bad lines, sprite DMA).
  Interrupts are sampled at the penultimate cycle of an instruction.
- The SIDs are clocked lazily: brought up to the current cycle on every
  register access and at the end of the block, each through reSIDfp's own
  sinc resampler at the host rate, into a short FIFO that absorbs the
  one-sample jitter between chips.
- Booting (the KERNAL's RAM test) runs in warp with the audio muted, under a
  per-block CPU budget so the host never sees a spike.

### Memory map extras

- Up to eight SIDs. SID 1 sits at `$D400` and mirrors through `$D400-$D7FF`
  except where another SID is placed. SIDs 2-8 go anywhere in `$D420-$D7E0`
  or `$DE00-$DFE0` in steps of `$20` (the addresses SID-Wizard, Cynthcart and
  SID Synth look for: `$D420`, `$D440`, `$D460`, `$D500`, `$DE00`, `$DF00`).
  A SID in the I/O pages and a MIDI cartridge on the same address is the
  user's conflict; the cartridge wins.
- Each SID has its own model and its own variant: 6581, 6581R3, 6581R4,
  6581R4AR, 8580R5. A variant is a starting point for the per-chip controls
  reSIDfp offers (filter curve, combined waveform strength, DAC leakage, 6581
  digi offset) -- the guesses are exposed as controls.
- Only SID 1 has the POT lines (paddles, 1351 mouse).

### MIDI cartridges (6850 ACIA)

| Interface | Control / Status | TX / RX | Interrupt | Notes |
|---|---|---|---|---|
| Sequential | `$DE00` / `$DE02` | `$DE01` / `$DE03` | IRQ | |
| Passport / Syntech | `$DE08` / `$DE08` | `$DE09` / `$DE09` | IRQ | |
| Datel / Siel / JMS / C-Lab / Kerberos / TeensyROM | `$DE04` / `$DE06` | `$DE05` / `$DE07` | IRQ | the default |
| Namesoft | `$DE00` / `$DE02` | `$DE01` / `$DE03` | NMI | |
| Maplin | `$DF00` / `$DF00` | `$DF01` / `$DF01` | IRQ | |
| Moog Song Producer | `$DE00` / `$DE00` | `$DE01` / `$DE01` | IRQ | |

The master-reset state matters: Cynthcart detects the cartridge by reading
status 0 after a reset and TDRE set after it is enabled (the reason its
autodetect fails in VICE). HerMIDI (IEC bus, SID-Wizard only) is out of scope
until the 1541's serial bus exists (phase 3).

### Disks

Two drive modes, per instance:
- **Virtual drive (default).** The KERNAL's serial routines (`TALK`, `LISTEN`,
  `SECOND`, `TKSA`, `CIOUT`, `ACPTR`, `UNTLK`, `UNLSN`) are trapped when the
  stock KERNAL is mapped, and a small DOS answers them from the attached
  `.d64`: load, save, sequential files, the directory, the command channel.
  Instant, and enough for every program here, which load and save through the
  KERNAL.
- **True 1541-II (phase 3).** A second 6502 with two VIAs, the GCR stream and
  the IEC bus, for programs with their own fast loaders.

The attached disk lives in the plugin state, including everything the program
saved to it, and can be exported as a `.d64`.

### Media and autostart

- `.prg`: injected when BASIC is ready, then `RUN` (or `SYS` from the load
  address when it is not `$0801`).
- `.d64`: attached as drive 8; autostart loads the first program (`,8,1`
  semantics) and runs it.
- `.crt`: normal 8K/16K/Ultimax, Ocean, Magic Desk, EasyFlash (read-only
  flash at first). Raw `.bin` of 8K or 16K as a normal cartridge.
- Files come in through the Load button or by dropping them on the screen.

### Input

- Keyboard: positional mapping from the host's scan codes, so a German or US
  keyboard both put the keys where a C64 has them. Esc is RUN/STOP, Page Up
  is RESTORE, the cursor keys map to the C64's two cursor keys with shift.
- Joystick: numpad (8/4/6/2, 0 or 5 fire) or the cursor keys with right Ctrl
  as fire, on port 1 or 2 -- the cursor keys then stop typing.
- Mouse: 1351 on port 1, and paddles (Cynthcart uses them) on the mouse.
- The window grabs the keyboard on a click into the screen.

### Plugin surface

- Audio: a stereo main output (all SIDs, per-SID level and pan) and eight
  mono aux outputs, one per SID.
- Note in: CLAP notes, CLAP MIDI and MIDI dialect events are turned into MIDI
  bytes. Optional MIDI clock, start/stop/continue and song position from the
  host transport, for programs that sync (SID Synth's arp, SID-Wizard).
- Note out: whatever the ACIA transmits, as CLAP MIDI events.
- Parameters (automatable): master volume, per-SID level and pan.
- State: a full machine snapshot (CPU, RAM, colour RAM, VIC, CIAs, ACIA,
  every SID's reSIDfp state, cartridge, virtual drive and disk image) plus
  the machine settings, so a project reopens with the program running as it
  was left, patches edited on the C64 included.

### GUI

The C64 screen (384 x 272, VICE's normal borders, scaled 1x-3x) with a strip
above it -- Load, Reset, Disk, Machine, MIDI, Joystick -- and a status line
with the drive LED and MIDI in/out activity. The machine page holds the video
standard, the SID table (count, address, model, variant controls, level,
pan), the MIDI interface, the drive mode, the palette (the VICE `.vpl` files
in `palettes/`, Colodore by default) and the ROM folder. The accent is a
Commodore-ish violet-blue, `#7C70DA` (the C64's own light blue), over a
graphite tinted towards it.

## Phases

1. **The machine** -- 6502 (all opcodes, decimal mode), PLA, VIC-II, CIAs,
   SID bank on reSIDfp, ACIA cartridges, keyboard/joystick, `.prg`, `.d64`
   via the virtual drive, raw and `.crt` cartridges, snapshots. A headless
   runner (`sidmidi-run`) that boots, autostarts a program, feeds it MIDI and
   key presses and writes a WAV and screenshots. CPU verified with the
   Klaus Dormann functional and decimal tests.
   *Done when* all four programs in `_dev_/` boot and play a note from MIDI
   in the runner.
2. **The plugin** -- CLAP glue, audio ports, note in/out, MIDI clock from the
   transport, state, parameters, and the window: screen, keyboard grab,
   joystick, file loading and drop, the machine page. VST3 and Windows like
   the other standalone plugins.
3. **The 1541** -- true drive emulation with the IEC bus, drive mode switch,
   HerMIDI. Mouse and paddles.
4. **Polish** -- per-chip 6581 filter range (a reSIDfp patch: one filter
   model per range value, shared read-only), manual, screenshots, demos.

## Status (0.1.0, 2026-11-09)

**Phase 1 is done.** The CPU passes Klaus Dormann's functional test.
Cynthcart, Station64, SID Synth and SID-Wizard boot and draw correctly in
`sidmidi-run`, on Linux and (cross-built) under wine. Cynthcart, Station64 and
SID Synth play MIDI notes at the right pitch, checked from the SID registers.
Cynthcart's cartridge autodetection finds the Datel interface. SID Synth's
menus answer the keyboard. Startup times: SID Synth about 4 s, Station64
about 12 s of machine time (its own unpacking).

**Phase 2 works but has not been in a DAW.** CLAP and VST3 build, Linux and
Windows. `sidmidi-selftest` passes: it loads the `.clap`, plays a note, and
round-trips the state into a second instance that plays on within -60 dB.
The window (screen, toolbar, machine page, keyboard grab, drop, file dialog)
has been checked on Xvfb only.

Cost: about 12 % of one core with one SID and 43 % with eight, most of it
reSIDfp's sinc resampler.

Open, in order:
- Load it in a DAW. Untested: keyboard focus, the VST3 wrapper's MIDI
  (CC, pitch bend), MIDI out, resizing.
- SID-Wizard's MIDI (its device has to be picked in its startup menu).
- The MIDI receive queue is a `std::deque` that can allocate in the audio
  thread; make it a fixed ring.
- A ROM folder in the settings, and the release decision on the embedded ROMs.
- Phase 3 and 4 as above. Also a cheaper resampling option for many SIDs.

## Status (0.2.0, 2026-11-09)

**Phase 3 is done, and phase 2 has its ROM folder.**
- ROMs: no longer compiled in. The plugin creates its ROM folder with a
  README, recognises the files by content, shows what is missing and where
  to put it, rescans every two seconds, and keeps a project's machine waiting
  until the ROMs are there.
- True 1541: VIAs, GCR (D64 to tracks and back), stepper, speed zones, byte
  ready on SO, writing, the serial bus with ATN auto-acknowledge, a drive
  that sleeps when idle. SID Synth and Station64 load through it; the stock
  1541 DOS saves a file that decodes back into the `.d64`; the state round
  trip passes with the drive in it. Loading warps while the motor runs
  (setting); in realtime warp takes at most half of each block's time, and
  offline (CLAP render extension) a fixed 24x.
- Mouse 1351 and paddles per control port, POT lines switched by CIA1 PA6/7.
- HerMIDI: an emulated PIC12F509 running the real firmware. SID-Wizard
  switches it on (its menu shows success), and a note sent through it plays
  an instrument of a tune loaded from disk.
- CPU: dormant SIDs (never written since reset) are not clocked; the VIC
  skips pixels when no window is open and no sprites need collisions; idle
  CIAs skip their timers. About 10 % of a core for the machine, plus about
  4.5 % per SID in use. LTO and fast-math were measured and bring nothing.

Open, in order:
- Load it in a DAW (CLAP and VST3).
- The MIDI receive queues are `std::deque`s that can allocate in the audio
  thread; make them fixed rings.
- Phase 4: per-chip 6581 filter range, manual, screenshots, demos.

## Next session

Where 0.2.0 was left (2026-11-09):
- **The user tests it in a DAW** (VST3 on this machine, CLAP on the other).
  Whatever comes back goes first. Untested so far: keyboard focus in the
  host's window, the VST3 wrapper's MIDI (CC, pitch bend, SysEx), MIDI out,
  resizing, mouse and paddles from the host mouse, the file chooser.
- The ROMs are installed for the user's own tests in
  `~/.local/share/SidMidi/ROMs` (copied from `_dev_/roms`, private use).
- `MidiCart` and `HerMidi` queue incoming bytes in `std::deque`s, which can
  allocate in the audio thread: replace with fixed rings.
- The user has not yet confirmed that HerMIDI's firmware may stay in
  `third_party/hermidi` (no licence of its own; SID-Wizard declares itself
  WTF-licensed).
- Phase 4: per-chip 6581 filter range (a vendored reSIDfp patch), the manual
  (`docs/manual.md` and SidMidi's own make-manual, the ROM instructions in
  it), screenshots, demos; then drop `NO-RELEASE`.
- Maybe: a cheaper resampling option for many active SIDs.
