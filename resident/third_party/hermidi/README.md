# HerMIDI firmware

`HerMIDI-PIC.hex` is the PIC12F509 firmware of HerMIDI 1.0c, a MIDI interface
for the C64's serial (IEC) port by Mihaly Horvath (Hermit Software Hungary,
2015), with its source `HerMIDI-PIC.gpasm`. Both come unchanged from the
SID-Wizard 1.93 source package, which its author declares WTF-licensed ("you
can do WTF you want with it").

ReSIDent compiles the hex in and runs it on an emulated PIC12F509
(`src/emu/Pic12F509.*`), so the emulated HerMIDI behaves exactly as the real
one: the init string, the synchronous, asynchronous and direct transfer modes,
the 25-byte buffer.
