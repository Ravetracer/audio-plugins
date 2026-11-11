# Patches for the third-party checkouts in `CLAP/`

`CLAP/` is gitignored — every checkout there is an independent upstream clone
that each developer makes themselves. Anything that has to be changed in one of
them therefore lives here as a patch, or it is lost on the next fresh clone.

Apply one with:

```sh
cd CLAP/<checkout>
git apply ../../shared/patches/<patch>
```

## `clap-wrapper-vst3-sdk-3.8.patch`

Against `free-audio/clap-wrapper` v0.16.0, needed to build against VST 3.8 or
newer — which is the first MIT-licensed VST3 SDK, and therefore the only one the
suite can use.

VST 3.8 added `FObject::iid`, and at the same time renamed the parameter of the
`DEFINE_INTERFACES` macro from `iid` to `_iid`. The wrapper's `wrapasvst3.h`
writes its dynamic `queryInterface` block by hand and still says `iid`, which
now resolves ambiguously between the macro's parameter and the new
`FObject::iid` member, so the wrapper does not compile. The patch spells those
six references `_iid`.

It is upstream's bug rather than ours, and has not been reported yet. The patch
makes the wrapper require VST3 3.8+; it will not build against 3.7 with it
applied, which is fine because 3.7 is not MIT.

## `clap-wrapper-auv2-main-bus-channels.patch`

Against `free-audio/clap-wrapper` v0.16.0 (still needed on main as of
v0.17.0), for the macOS Audio Unit. Without configurable audio ports, the
wrapper's `SupportedNumChannels` listed the channel counts of *every* audio
port as possible layouts of the main bus. A plugin with a stereo main output
and mono auxiliary outputs -- ReSIDent, with one output per SID -- then
advertised a mono main output it cannot be set to, and `auval` failed its
format tests ("Cannot Set Output Num Channels:1 when unit says it can"). The
patch lists only the main port's count when there is a main port.

Upstream's bug, not reported yet. Only the AUv2 build compiles the file, so it
changes nothing on Linux or Windows.
