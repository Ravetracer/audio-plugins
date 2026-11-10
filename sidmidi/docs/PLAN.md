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
  in `third_party/libresidfp/` with its `COPYING`; our changes to it are
  kept as patches in `third_party/patches/` and listed in
  `third_party/README.md`. The rest of the
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
- Parameters (automatable): master volume, per-SID level and pan, and 64
  CC slots (0.3.0, `plugin/CcMap.*`; Station64 alone has 53): ids 1000-1063, range 0..127, named by
  the CC map in use and renamed with `CLAP_PARAM_RESCAN_INFO | TEXT` (the
  VST3 wrapper turns it into `kParamTitlesChanged`; ranges cannot change
  there, so they never do). A slot's move becomes a CC at its sample
  offset, sent only when its integer value changed; restoring values
  (project load, a new map) sends nothing. Factory maps are
  `ccmaps/*.ccmap`, embedded; the user's live in `CC Maps` beside the ROM
  folder; the map in use is saved in the project in full.
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
4. **Polish** -- per-chip 6581 filter range (a reSIDfp patch), manual,
   screenshots, demos. Done in 0.4.0; see its status.

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

## Status (0.2.1, 2026-11-10)

Fixes, no new features:
- **No allocation in the audio thread while playing.** The MIDI cable
  queues (`MidiCart`, `HerMidi`) and the cartridge's transmit queue are
  fixed rings (`emu/FixedQueue.h`, 4096 bytes in, 1024 out); a MIDI message
  that does not fit is dropped whole -- a cut one left a stray status byte
  that Cynthcart's parser never recovered from. The plugin's event list and
  MIDI out stop at their reserved capacity, the event sort is an in-place
  insertion sort (`std::stable_sort` takes a heap buffer), and the mix
  buffer is sized at `activate()`. The self-test counts allocations inside
  `process()` with its own `operator new` and floods 9000 bytes of MIDI
  into one block: 0.2.0 allocated 2 times playing and 290 times in the
  flood, 0.2.1 never. Loading a file through the virtual drive still
  allocates (its DOS works in strings and vectors); the count starts once
  the program runs.
- **Offline renders repeat exactly, and instances are independent.**
  reSIDfp dithers its filter's float-to-int conversion from a 1024-value
  noise table whose position was one `mutable` index in the process-wide
  model config: its tables are built on six threads at once (a race, so
  they differed from run to run by an LSB), and every chip of every
  instance advanced the same index per sample. The index is now per filter
  and in reSIDfp's `State` (patch `third_party/patches/0001-...`). Two
  renders are bit-identical (`install.sh` checks it), Linux and Windows
  render the same bits, and the state round trip is exact (it was within
  -60 dB). A 0.2.0 snapshot's SID state no longer matches in size and
  takes the existing fallback: a reset with the registers written back.

## Status (0.3.0, 2026-11-10)

**CC maps.** 64 automatable CC slots, named by a map per program:
- Factory maps (`ccmaps/`) for SID Synth (36, with every value label),
  Station64 (53), Cynthcart (13) and SID-Wizard (3). The CC numbers and the
  value ranges were read from the manuals and, where those are silent, from
  the programs' own handlers (SID Synth and Station64 disassembled from a
  memory dump, Cynthcart and SID-Wizard from their sources).
- Loading a program picks the map whose `programs` occur in the file name
  (*Follow program*, on by default); the user's maps win over the factory's.
- The CC page: map, follow, channel, save as, folder; per slot the CC (with
  `@channel`), the name with value labels after `|`, and the value.
- Self-test: names and labels through the CLAP API, one message per move
  and none for an unchanged value or an unused slot, no allocation, the map
  in the state, and heard: the Volume slot at 0 and 127 (CC 7 is the volume
  in Cynthcart and Station64).
- SID Synth was driven end to end in `sidmidi-run`: menu item 01 to DATEL,
  then CC 74 and 71 at 0 and 127 move its CUTOF and RESO bars.

Worth knowing about the programs (all in the factory maps' comments):
SID Synth's MIDI is off until menu item 01 selects an interface; Cynthcart
treats CC 16-31 as 0-15 and ignores 32 and up; Station64 listens on
channels 1-3 and treats CC 64-127 as 0-63 (so a host's CC 121 "reset all
controllers" switches its LP filter off); SID-Wizard stops its tune on
CC 121.

## Status (0.4.0, 2026-11-10)

**Phase 4 is done, and SidMidi is released** (no `NO-RELEASE` any more).
- **Per-chip 6581 filter range and caps** (reSIDfp patch 0002). The range
  and the 2200 pF caps only change two numbers of the shared 6581 model
  (the integrators' current factor and the VCR multiplier), so each
  `Integrator6581` carries its own pair and the tables stay shared -- not
  one model per range value, as planned. The default (19/39 = reSIDfp's
  uCox of 20e-6) renders bit-identically to 0.3.0. On the machine page per
  chip as Range and Caps; in the settings text as two more fields of the
  `sidN` line. After a snapshot restore SidBank applies the settings' range
  and caps again (the chip state carries its own); not the curve, whose
  re-application recomputes a cutoff voltage the state restored exactly.
- **A bug the patch also fixes**: reSIDfp's `Params` were never
  initialised, and restoring a chip state applied their `filterRange6581`
  and `old6581caps` to the shared model -- so opening a project changed
  the 6581 filters of every SidMidi in the process. The round-trip test
  could not see it (both instances shared the change); it now records A's
  phrase before B loads, and a C with another range and a D with the
  original state prove the chips independent (0.262 max difference before
  the patch, 0 after).
- **Resampling: Sinc (default) or Fast** (reSIDfp DECIMATE), a machine
  setting. Measured with eight playing SIDs from BASIC: 37.8 % vs 33.7 %
  of a core at 48 kHz, 41 % vs 34 % at 96 kHz, no difference at 192 kHz;
  one SID's clocking is most of its cost, so Fast saves a tenth to a fifth.
- **Manual** (`docs/manual.md`, `tools/make-manual.sh` with its own
  `tools/docgen.cpp`: the parameter table from `plugin/Params.cpp`, the CC
  map library from `ccmaps/` in place of a preset library), **screenshots**
  (`tools/make-screenshots.sh` on `sidmidi-gui-snapshot`, the window
  rendered offscreen; the ROM page shot runs with HOME=/home/user so no
  developer path appears), **demos** (`tools/make-demos.sh`: Cynthcart, SID
  Synth and Station64 playing a phrase, to dist/demos/SidMidi).
- **HerMIDI's firmware stays in `third_party/hermidi`** (the user's
  decision, 2026-11-10).

## Status (0.5.1, 2026-11-10)

0.5.1 is a fix: Station64 hung at its intro with a Sequential or Namesoft
interface (with or without a cartridge). Its test that tells the two apart
loads the control word from zero page (`LDA $31`, where `LDA #$31` was
meant); $31 is BASIC's STREND, $F7 after LOAD and RUN, a master reset, and
it then waits for TDRE. The chip reads TDRE as 0 in reset (MAME; Cynthcart's
autodetection depends on it and is "incompatible with VICE" by its own
source), VICE reads 1. `MidiCart::write` now does both: an ACIA held in
reset since power-on reads 0, one that was running and is reset again keeps
TDRE. Station64 then reports Namesoft for both, and both play.
`tools/interface-test.sh` (run by install.sh) plays a note through Station64
on all four interfaces and through Cynthcart on its three; reading 0 always
fails the first two Station64 cases, reading TDRE always fails every
Cynthcart case.

From the user's DAW use (CC renaming, Range/Caps and Fast resampling all
confirmed working there):
- **Disks apart from programs.** Media > Insert disk puts a .d64 into the
  drive and changes nothing else; Media > Start from disk runs a file of it
  (`C64::autostartDisk`, LOAD"name",8,1 and RUN typed, the name kept in
  `autostartPrg_` so a snapshot carries it). Loading or dropping a .d64
  inserts it and offers its files (leave it in, the first, or one by name;
  over twenty in submenus). A cartridge and a disk can be in at once;
  starting a .prg or a disk file takes the cartridge out (it would start
  instead). The plugin keeps the disk (`diskFileName_/Data_/Start_`) apart
  from the program or cartridge (`lastFileName_/Data_`): Power restarts the
  program, the cartridge, or the disk file that was started, and leaves an
  only inserted disk at READY; a project keeps both (three state blobs; a
  0.4.0 project whose one file was a .d64 is migrated), and the fallback
  restart (`restartMedia()`) restores both. Raw cartridge ROMs shorter than
  8K/16K that start with CBM80 are padded with $FF (Cynthcart's cartridge
  ROM is 8145 bytes). `sidmidi-selftest --media` proves a cartridge runs on
  bit-identically with a disk inserted, both survive the state, an inserted
  disk starts nothing and Start from disk runs the program.
- **Symbolic keyboard** (default) beside the positional one, a Machine page
  setting. The backends pass the layout's keysym with every raw press (X11:
  XLookupString; Win32: ToUnicode at WM_KEYDOWN with Ctrl taken out unless
  it is AltGr, dead keys as their character); `Keymap` resolves a key when it
  goes down to a C64 key and a SHIFT rule (as the host's, pressed, or held
  back), so Shift+, = ';' on a German layout reaches the C64 as ';'.
  `sidmidi-keytest` checks the German cases; typed end to end through X11
  and through the Windows build under wine (`PRINT "@"` with Shift+2 on a US
  layout; Xvfb in the devbox will not load another layout).

Also in 0.5.0, from the user's request for fast-loader cartridges:
- **Freezer cartridges** (`emu/Cartridge.*`): Action Replay (CRT 1: four 8K
  banks, 8K RAM at ROML and I/O 2, register $DE00, freeze = Ultimax + RAM +
  NMI, as VICE's actionreplay.c), Final Cartridge III (3: four 16K banks,
  register $DFFF -- bit 4 EXROM, bit 5 GAME, bit 6 NMI, bit 7 hide; its ROM
  answers in I/O 1 and 2) and Final Cartridge I (13: I/O 1 access off, I/O 2
  on); raw 32K Action Replay images, with or without a 2-byte header; raw
  ROM images shorter than 8K/16K with CBM80. Their state is an appended
  snapshot section (`CartFreezerState` and the RAM), so older snapshots
  still load. Media > Freeze. All thirteen images in `_dev_/cartridges`
  boot; freeze and restart were tried on the Action Replay.
- **I/O sharing**: the Final Cartridges run code from I/O 1, where the Datel
  MIDI interface sits ($DE04-$DE07). While the PC is in $DE00-$DFFF the
  cartridge answers reads; data reads go to the MIDI interface -- an ideal
  expander. (The FC3 hung at boot until this: its RTS at $DE04 read as the
  ACIA's 0, a BRK.) Writes to I/O 1/2 with a freezer present are split by
  writer (`C64::freezerWriting`): code in I/O 1/2, in the cartridge's ROM,
  or in RAM while the cartridge is mapped in (the AR's fast loader) reaches
  only the cartridge; anything else never does. Without it Station64's
  interface probe (a 6850 reset, $03, to $DE04, $DE08 and $DE00) switched
  the Action Replay into Ultimax and its freezer, and the AR's own $DE00
  writes went to a Sequential or Namesoft ACIA, so it could not boot.
  `tools/cart-test.sh` covers both.
- **The serial bus timing** (`C64::endCycle`): a C64 write to $DD00 reaches
  the drive one cycle after the write, as VICE passes CIA writes with clk+1
  (`c64cia2.c`, `store_ciapa`). Without it the Action Replay's fast loader
  read the first bit pair of some bytes a slot late (a sector number
  shifted right by two bits) and looped for ever; with it both fast loaders
  load Station64 byte for byte. A queue that held back the drive's own
  writes was tried first and made it worse; VICE has none.
- Loading a freezer while Drive 8 is Virtual switches it to the true 1541
  (their fast loaders run code in the drive). `tools/cart-test.sh` (in
  `install.sh`) loads Station64 through both fast loaders and the Action
  Replay without one and compares the C64's memory with the .prg; with the
  old bus timing the Action Replay case hangs and fails. The runner has
  `--freeze`, `--dump`, `--no-disk-warp` for this. Keys held during a warped
  boot are not seen by the FC3 (hold RUN/STOP with `--no-disk-warp`).

## Status (0.6.0, 2026-11-10)

From the user's DAW use: a game with its own loader, on the True 1541,
went into warp by itself and the Warp button could not stop it. Both warps
were by design (start, and the true drive's motor, `Engine::autoWarping()`),
but the disk one had only a state key (`warp_disk`) and no control, and the
button only cleared its own flag.
- **Auto warp** on the Machine page: start and disk, start only, disk only,
  off (`warp_boot`, `warp_disk` in the settings; a project without
  `warp_boot` keeps the start warp).
- **The Warp button stops an automatic warp** (`Engine::holdAuto_`) until it
  would have ended, i.e. the motor stops or the program has started; a new
  start (anything that calls `Engine::resync()`) warps again. Settings
  changes re-prime the audio through `resyncAudio()` and leave a stopped
  warp stopped.
- The private loader extension is `/4` (`warping`, `set_warp`);
  `sidmidi-selftest` checks that a start warps, the button stops it, the next
  load warps again and Auto warp off loads in real time. Put back, the
  button check fails.
- The status bar shows "SidMidi <version>" left of the indicators, so the
  user can tell which build a host has loaded.

## Next session

Where 0.5.0 was left (2026-11-10):
- **Feedback from DAW use** goes first. Untested in a host: the symbolic
  keyboard on a real German layout, keyboard focus in the host's window, the VST3
  wrapper's MIDI (CC, pitch bend, SysEx), MIDI out, resizing, mouse and
  paddles from the host mouse, the file chooser.
- The ROMs are installed for the user's own tests in
  `~/.local/share/SidMidi/ROMs` (copied from `_dev_/roms`, private use).
- Maybe: make the virtual drive allocation-free (fixed buffers for its
  channels, names and directory) so a program loading a file mid-song
  cannot allocate in the audio thread either.
- Maybe: a SID-Wizard demo (needs its MIDI set up in its menu first).
