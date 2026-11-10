# SidMidi

A Commodore 64 inside a plugin. SidMidi emulates the machine closely enough
that the original MIDI programs written for it — Cynthcart, Station64,
SID-Wizard, SID Synth and anything like them — run unmodified. Notes from the
DAW go in through an emulated MIDI cartridge, the program plays the SID chips,
and the SIDs play into the DAW. Whatever the program sends back out of the
cartridge arrives in the DAW as MIDI.

It is not a SID synthesizer with a C64 skin. The instrument is the C64
program; SidMidi is the computer it runs on.

**Status: 0.7.0.** Released for Linux and Windows, CLAP and VST3. The manual
is `docs/manual.md` (the PDF ships in the release archive); the plan and its
phases are in [docs/PLAN.md](docs/PLAN.md).

## What is emulated

- **6510 CPU** — every opcode including the undocumented ones, NMOS decimal
  mode, cycle-exact bus accesses (dummy reads, read-modify-write double
  writes), interrupts sampled where the chip samples them. Passes Klaus
  Dormann's 6502 functional test.
- **VIC-II** — 6569 (PAL) and 6567R8 (NTSC), cycle-stepped: bad lines that
  stop the CPU, sprite DMA, all graphics modes, sprites with priorities and
  collisions, the border flip-flops, raster interrupts.
- **Two CIAs** — timers with the chip's start and load delays, time of day,
  the serial register, keyboard matrix and joysticks.
- **Up to eight SIDs** — reSIDfp, each chip with its own address (`$D400`,
  `$D420` … `$D7E0`, `$DE00` … `$DFE0`) and its own model: 6581, 6581R3,
  6581R4, 6581R4AR, 8580R5, with filter curve, 6581 filter range, the early
  2200 pF filter caps and combined-waveform strength adjustable per chip,
  and a choice of sinc (default) or fast resampling.
- **MIDI cartridges** (6850 ACIA) — Datel/Kerberos/TeensyROM, Sequential,
  Passport/Syntech, Namesoft (NMI), Maplin, Moog Song Producer. The receive
  line runs at the cable's 31250 baud.
- **HerMIDI** — Hermit's MIDI interface on the serial port (SID-Wizard,
  Station64), as the real thing: an emulated PIC12F509 running HerMIDI's own
  firmware, in all three transfer modes.
- **Drive 8** — either the **virtual drive** behind the KERNAL's serial
  routines (LOAD, SAVE, files, the directory and the command channel from the
  attached `.d64`, instantly), or the **true 1541**: its own 6502 with the DOS
  ROM, both VIAs, the disk as GCR tracks with the speed zones, the stepper and
  the serial bus, for programs with their own loaders. Loading on the true
  drive runs in warp. What a program saves goes into the disk image, which
  lives in the project and can be exported.
- **Control ports** — joystick (on the keyboard), the 1351 mouse and paddles
  (on the host mouse over the screen), with the SID's POT lines switched
  between the ports as on the real machine.
- **Cartridges** — `.crt` (normal 8K/16K/Ultimax, Ocean, Magic Desk, Fun Play,
  Super Games, EasyFlash, Simons' BASIC) and raw 8K/16K `.bin` (shorter ROM
  images with the CBM80 signature are padded); the freezers Action Replay
  4.2-7.5 (also as a raw 32K `.bin`), Final Cartridge III and Final
  Cartridge I with their RAM, banking and freeze button. Their fast loaders
  run on the true 1541; loading one switches Drive 8 to it.

## Using it

- **Load** a `.prg`, `.d64`, `.crt` or `.bin`, or drop it on the window. It
  starts by itself (booting runs in warp, muted); a disk goes into the drive
  and offers its files: leave it in, run the first, or pick one.
- **Media** inserts a disk without starting anything (a cartridge and a disk
  can be in at once), starts a file from the inserted disk, exports the disk
  and ejects disk or cartridge.
- Pick the **MIDI** interface the program expects (Datel is the most common,
  and the one Kerberos and TeensyROM emulate). Many programs need MIDI
  switched on in their own menu, too.
- **Click into the screen** to type on the C64. *Keyboard* on the Machine
  page is **Symbolic** (default: a key types the character the host's layout
  prints on it, with SHIFT pressed or held back as the C64 needs) or
  **Positional** (a key is the C64 key in its place, whatever the layout).
  Keys without a character, and the positional layout:

  | Host | C64 | Host | C64 |
  |---|---|---|---|
  | Esc | RUN/STOP | Tab | CTRL |
  | Left Ctrl | C= | ` (grave) | ← |
  | - = | + - | [ ] | @ * |
  | ; ' | : ; | \ or # | = |
  | Home | CLR/HOME | Insert | INST |
  | Delete / Backspace | DEL | End, <> | £ |
  | Page Down | ↑ | Page Up | RESTORE |
  | F1–F8 | F1–F8 (even = shifted) | Cursor keys | the C64's cursor keys |

- **Joy** puts a joystick on the numpad (0 or 5 = fire) or on the cursor keys
  (right Ctrl = fire), in port 1 or 2.
- **Mouse and paddles**: set port 1 or 2 to *Mouse 1351* or *Paddles* on the
  Machine page; the host mouse over the C64 screen then moves it (left and
  right button are the buttons; for paddles the pointer's X and Y are the
  two knobs).
- **Machine** holds the video standard, the palette, MIDI clock from the
  host's transport, the drive (Virtual, True 1541, None), the control port
  devices, and the SIDs: how many, where, which model, their filter, level
  and pan.
- **CC** shows the CC map: the controllers the program listens to, as 64
  automatable parameter slots in the host. Pick a map (SID Synth and the
  other supported programs have one built in, and loading a program picks
  its map when *Follow program* is on), or make your own: click a slot's
  controller to set it (`74`, or `74@2` for channel 2), its name to rename
  it, with value names after a `|` (`Vib On | 0 Off, 64 On`). *Save as*
  stores the map as a text file in `~/.local/share/SidMidi/CC Maps`
  (`%APPDATA%\SidMidi\CC Maps` on Windows). Moving a slot, by hand or by
  automation, sends its controller to the C64 at that exact sample. The
  slots keep their parameter ids, so automation stays put when the map
  changes; the host shows the new names.
- **Warp** runs the machine flat out, muted — for programs that take a while
  to unpack themselves (Station64 needs about twelve seconds).
- The project saves the whole machine: reopening it puts the program back
  exactly where it was, with everything edited on the C64.

### Outputs

A stereo main output with every SID panned in, and one mono output per SID.
With **Voice outputs** on (machine page), each voice of every SID also has an
output of its own, 24 in all: three more reSIDfp instances per SID, each
hearing one voice, so every voice keeps the chip's filter. It costs about
four times the processing per SID in use.

## The ROMs

SidMidi needs the Commodore 64's ROMs, which are copyrighted and **do not
come with SidMidi**. On its first start it creates its ROM folder and shows
what to put there:

- Linux: `~/.local/share/SidMidi/ROMs` (or `$XDG_DATA_HOME/SidMidi/ROMs`)
- Windows: `%APPDATA%\SidMidi\ROMs`

| ROM | VICE's file name |
|---|---|
| KERNAL | `kernal-901227-03.bin` |
| BASIC | `basic-901226-01.bin` |
| Character set | `chargen-901225-01.bin` |
| 1541 DOS (only for the True 1541) | `dos1541ii-251968-03.bin` or `dos1541-325302-01+901229-05.bin` |

The VICE emulator has them in its `C64` and `DRIVES` data folders, or dump
them from your own C64. Any file names work; SidMidi recognises the ROMs by
their content and starts the machine as soon as they are there.

## CPU

About 10 % of one core with the window closed (a few percent more with it
open), plus about 4.5 % for every SID the program actually plays. SIDs that
are configured but never touched cost nothing.

## Building

```sh
./install.sh      # build in ../devbox.sh, run the tests, install the CLAP and the VST3
```

The ROMs are never part of the build. The tests take them from `_dev_/roms/`
(gitignored) through `SIDMIDI_ROM_DIR`; the installed plugin uses its ROM folder.

Tools in `build/`: `sidmidi-run` (the machine without a host: autostart a
program, play it a MIDI script, write a WAV and screenshots), `sidmidi-selftest`
(the built `.clap` loaded like a host loads it, with a state round trip),
`sidmidi-cputest`, `sidmidi-gui-host` and `sidmidi-gui-snapshot` (the
window rendered offscreen into a PNG).

Release material, all run in the devbox and writing only into `build/` and
`dist/`: `tools/make-manual.sh` (the PDF manual, with its own
`tools/docgen.cpp`; `release.sh` calls it), `tools/make-screenshots.sh`
(`docs/images/`, offscreen) and `tools/make-demos.sh` (MP3s into
`dist/demos/SidMidi/`, from the programs in `_dev_`).

## Licence

**GPL-2.0-or-later** — this folder only. The SID emulation is reSIDfp from
[libresidfp](https://github.com/libsidplayfp/libresidfp) 1.2.3 (Dag Lem,
Antti S. Lankila, Leandro Nini), vendored in `third_party/libresidfp` with
three patches of ours (`third_party/patches/`), under the GPL, which makes SidMidi as a whole GPL. The palettes in `palettes/`
are VICE's (GPL). HerMIDI's firmware in `third_party/hermidi` is Mihaly
Horvath's, from the WTF-licensed SID-Wizard package. The rest of this
repository is MIT.

Commodore 64, C64 and SID are names of products of their respective owners;
SidMidi is not affiliated with or endorsed by any of them. The C64 programs
mentioned are their authors' and are not included.
