# ns7driver: Numark NS7 on Apple Silicon

A driver for the original **Numark NS7** (USB `15e4:0071`) on current macOS,
including M1/M2/M3 Macs. It runs entirely in user space: **no kernel
extension, SIP stays on**, no Reduced Security.

- **Audio:** "Numark NS7" appears as a 4-in / 4-out, 24-bit, 44.1 kHz device
  in every app (outputs 1/2 master, 3/4 cue; inputs 1–4).
- **MIDI:** an "NS7" MIDI input and output for Serato's MIDI mapping, Mixxx,
  rekordbox, Bome, etc. LEDs work through the output.
- **Clock:** CoreAudio follows the NS7's own crystal (measured from the USB
  feedback pipe), so there's no drift or periodic clicks over a long set.

> **Status:** built and unit/simulation-tested, **not yet run on a real NS7.**
> Expect the first hardware session to need a fix or two. The test commands
> below are designed to find out quickly what works.

## One-click install

1. Unzip, open the folder, and double-click **Install NS7 Driver.command**.
   The first time, macOS may block it because it's from the internet: right-click it,
   choose **Open**, then **Open** again.
2. Enter your Mac password when asked. It installs Apple's command line tools and
   Homebrew if you don't have them, then builds, tests and installs the driver.
3. Plug in the NS7. "Numark NS7" appears in your audio settings within a few seconds.

To remove it, double-click **Uninstall NS7 Driver.command**. The install log is in
`~/Library/Logs/ns7driver-install.log`.

## How it fits together

```
Serato / Mixxx ──CoreAudio──▶ NS7Audio.driver ─┐            ┌─ NS7 MIDI ports ◀── ns7d midi-helper
                                (plug-in)      │ shared     │                     (your login session)
                                               ├─ memory ───┤
                                               │ (XPC)      │
                                    ns7d serve ┘            └──────────────────────────┘
                                 (launchd service, owns USB via libusb)
                                        │
                                   Numark NS7
```

## Before installing: test on the hardware

```sh
brew install libusb pkg-config cmake
make && make test
./ns7d info              # USB layout, read-only; send me this output
./ns7d -v midi           # setup + streaming; move controls, see MIDI printed
./ns7d tone 5            # 440 Hz on master (1/2), 660 Hz on cue (3/4)
./ns7d play song.wav --cue
./ns7d record in.wav 10  # records inputs 1-4 (mic/aux) to a WAV
```

`run`/`midi` print a status line every 2 s:

```
out 44100 fr/s  in 44100 fr/s  feedback ok  underruns 0  midi in 312 out 0  errors 0 | in dB: -inf -inf -42.1 -40.3
```

What to look for:

| Line shows | Meaning |
| --- | --- |
| `out`/`in` ≈ 44100 | audio is streaming both ways |
| `feedback ok` | the output follows the NS7's clock |
| `midi in` rising as you move controls | controls work; `midi` mode prints each message |
| `errors` rising | send me the `-v` output |

If setup fails, run it with `-v` and send the output.

## Install

```sh
./install.sh
```

This builds everything, runs the tests, installs:

| What | Where |
| --- | --- |
| `ns7d` | `/usr/local/bin/ns7d` |
| NS7 Audio plug-in | `/Library/Audio/Plug-Ins/HAL/NS7Audio.driver` |
| driver service | `/Library/LaunchDaemons/com.ns7driver.daemon.plist` (log: `/Library/Logs/ns7d.log`) |
| MIDI helper | `/Library/LaunchAgents/com.ns7driver.midi.plist` (log: `/tmp/ns7-midi.log`) |

and restarts CoreAudio. Plug in the NS7; "Numark NS7" shows up in a few
seconds and disappears when it's unplugged. If macOS asks to allow the
accessory, choose **Allow**. Remove everything with `./uninstall.sh`.

The test commands need the USB device to themselves, so stop the service
first: `sudo launchctl bootout system/com.ns7driver.daemon` (start it again
with `sudo launchctl bootstrap system /Library/LaunchDaemons/com.ns7driver.daemon.plist`).

### Serato

Serato recognises a *native* NS7 by its own driver, so with this driver
the NS7 shows up as a generic audio device plus MIDI controller. Use Serato DJ
Pro's MIDI mapping (needs a license) or **Mixxx** (free; has a built-in NS7
mapping). Point the app's audio output at "Numark NS7", master on 1/2 and cue
on 3/4.

## Latency

About 2 ms per USB transfer, 3 in flight: output reaches the NS7 roughly
6 ms + the safety margin (≈4.7 ms) + your app's buffer after the app renders
it. CoreAudio is told these figures, so apps compensate. Controls reach MIDI in
1–2 ms.

## Protocol

The NS7 uses Ploytec's USB audio chip ("isochronous family"). What the
driver does, in `src/ns7_proto.h` and `src/ns7_usb.c`:

- **Setup:** firmware read ('V'), 200 ms wait, status read ('I'), clock
  source automatic, sample rate 44.1 kHz on both audio endpoints (UAC
  SET_CUR), read back, then the status confirm write that arms streaming.
- **Audio out:** isochronous, plain 24-bit little-endian, 4 channels; frames
  per packet follow the device's 3-byte feedback packets.
- **Audio in:** bulk/interrupt transfers of 64-byte frames, each channel's 24
  bits spread across 24 bytes (one bit lane per channel pair).
- **MIDI:** own endpoints, 42-byte transfers each way every millisecond: 41
  MIDI bytes (0xFD = empty) + a control byte in; up to 4 bytes + 0xFD padding
  + 0xE0 out.

Verified directly in Numark's NS7 Mac driver 3.3.11 (`NumarkNS7Audio.kext`,
by Ploytec): USB IDs, the 42-byte MIDI framing (`kInACR 8`,
`AJ::inACR_NS7`, `USBMidiPattern::initNS7`), the 200 ms setup wait,
24-bit/44.1 kHz/4-channel format. The remaining protocol details come from
the [Ozzy](https://github.com/mischa85/Ozzy) project's reverse engineering
of Ploytec devices (MIT); the input decoder is cross-checked against Ozzy's
in `make test OZZY=/path/to/Ozzy`.

## Tests

`make test` runs without hardware:

- `test_proto`: frame codec round trips and bit placement, rate/status
  encoding, MIDI framing and pacing, MIDI message assembly (running status,
  real-time, SysEx, split transfers), feedback pacing, ring buffer, WAV I/O.
- `test_timeline`: simulates the NS7 (off-nominal crystal, ±1 ms USB
  jitter), the USB thread and CoreAudio together for 20 s per scenario, and
  checks every output and input frame lands intact and the clock tracks the
  true rate.

## Files

```
src/ns7_proto.h      protocol constants, framing, codecs, feedback pacing
src/ns7_usb.c        libusb engine: setup, isoc/bulk/interrupt streams
src/ns7_shm.h        shared memory layout + clock (used by ns7d and the plug-in)
src/ns7_serve.c      `serve` (launchd service), `midi-helper`, XPC transport
src/ns7_midi.c       MIDI message assembly, CoreMIDI ports
src/ns7d.c           command line: info / run / midi / tone / play / record
plugin/NS7Audio.cpp  CoreAudio plug-in (libASPL)
macos/*.plist        launchd jobs
tools/ns7_probe.py   standalone USB probe (pyusb)
```

## Security note

Any local process can connect to the `com.ns7driver.daemon` service and use
the shared audio/MIDI memory. That's fine on a personal machine; don't
install this on a shared one.

## Credits

- Protocol research: [Ozzy](https://github.com/mischa85/Ozzy) by Marcel Bierling (MIT)
- CoreAudio plug-in framework: [libASPL](https://github.com/gavv/libASPL) by Victor Gaydov (MIT)
- USB: [libusb](https://libusb.info) (LGPL-2.1, linked dynamically)

License: MIT (see LICENSE).
