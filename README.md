# AES67 macOS Audio Driver

> **EXPERIMENTAL SOFTWARE — USE WITH CAUTION**
>
> This is a work-in-progress open-source AES67 audio driver for macOS. An earlier version of the RX (receive) path received audio from real AES67 hardware (a Riedel Artist intercom system) into Reaper in short tests. Since then, the receive path and device clock have been rebuilt around a single media clock ([step 2](#step-2-clocking-redesign-in-progress)). The rebuilt path is verified in Core Audio with test senders, but not yet with the Riedel. The TX (transmit) path has not been tested with real hardware.
>
> **Production use is not recommended without thorough testing in your environment.** This project is under active development.

A work-in-progress open-source virtual audio driver for macOS that aims to provide AES67 network audio support. Built as a user-space AudioServerPlugIn using the libASPL framework.

## Current Status

**Verified with real hardware (before step 2):**

- RX from a Riedel Artist intercom system via AES67 multicast, recorded in Reaper
- Multicast interface binding on a multi-NIC machine (binds to the specified interface)
- L24 at 48 kHz with 1 ms packets

**Verified in Core Audio with test senders (step 2, October 2026):**

- Received audio is placed by RTP timestamp at a fixed 8 ms latency. Two streams arrive sample-aligned, and two apps recording at once receive identical input.
- The device clock follows the received stream. With a sender 200 ppm fast, Core Audio's measured device rate locked to +200 ppm, with no breaks in the audio.
- Sender stalls of up to 23 ms play as short silences without moving the timeline.

**Next:** repeat the RX test on the Riedel with the rebuilt path, for 1 hour with a test tone.

## Step 1: Critical-Path Fixes

A full technical review in October 2026 found several defects on the critical audio path. These were fixed, each with a regression test in `Tests/TestCriticalPathRegressions.cpp` that failed before the fix. Step 2 then replaced the receive pacing machinery (consume thread, jitter buffer, rate controller) with timestamp placement. The receive fixes below keep their regression tests, rewritten against the new design:

- **TX sent only silence.** libASPL delivers mixed output through `OnWriteMixedOutput` (`DeviceParameters::EnableMixing` defaults to true); the driver only implemented `OnWriteClientOutput`, which is never called in that mode.
- **The device ran at 44.1 kHz.** libASPL's default nominal rate is 44.1 kHz while the streams declared 48 kHz, and rate changes from Audio MIDI Setup never reached the driver. The device now starts at 48 kHz, offers only rates the active streams can deliver, and applies rate changes to the driver, StreamManager and both streams' formats.
- **The RX pacing loop ran away.** It measured the device ring buffer, which the consume thread cannot regulate, and ran itself to its +0.5% clamp within about 10 seconds, causing periodic underruns and steadily growing latency. Step 1 fixed the loop; step 2 removed it, since latency is now fixed by timestamp placement.
- **A sender restart caused up to 65 seconds of silence.** The receiver now follows a new source once its packets agree for 50 ms (step 2; step 1 used 4 packets, which a stalled sender could fake). It ignores single late packets and second senders on the same group.
- **Lost packets shifted the timeline.** A lost packet now plays as one packet of silence, so streams stay sample-aligned with each other.
- **Saved TX streams reloaded as RX streams.** Stale jitter buffer slots could also block newer packets; the jitter buffer is gone in step 2.
- The plug-in now declares `AudioServerPlugIn_Network`, which Apple requires for network access from the sandboxed audio host.

## Step 2: Clocking Redesign (In Progress)

AES67 needs every device on one PTP-derived media clock. Step 2 replaces the driver's independent timers with a single media clock that drives Core Audio's sample clock, places received audio by RTP timestamp, and stamps transmitted packets. The full plan is in [Docs/Step2-Clocking-Plan.md](Docs/Step2-Clocking-Plan.md).

| Stage | Status | Result |
|---|---|---|
| Spike S1: PTP inside the sandboxed driver host | Done | The driver host (`_coreaudiod`) can bind UDP 319/320, join the PTP group and get kernel receive timestamps in Core Audio's time base. Its threads can run ~30 ms late, so PTP must use kernel timestamps. [Write-up](Docs/Spikes/S1-PTP-Sandbox.md) |
| Spike S2: Core Audio following a computed clock | Done | Core Audio tracked a model-driven clock to within 0.1 ppm (constant) and 3 ppm (ramp), and resynchronised cleanly on a phase step. [Write-up](Docs/Spikes/S2-HAL-Clock.md) |
| Phase 0: clock foundations | Done | `NetworkEngine/Clock/`: lock-free `MediaClock`, exact host-time conversion, and a timestamp-indexed audio buffer, tested deterministically and under ThreadSanitizer |
| Phase 1: device clock from the media clock | Done | The driver computes Core Audio's zero timestamps from its own media clock. Verified in the real HAL: +0.0 ppm, no timeline jumps or overloads, unbroken RX audio. The clock then ran on the Mac's host clock; phase 3 adds a recovered source |
| Phase 2: receive by RTP timestamp | Done | Received audio is placed by RTP timestamp at a fixed link offset (8 ms, matching the Riedel's 8 x packet time receive buffer), so latency is fixed and streams on one network timeline are sample-aligned. The jitter buffer, consume thread and rate controller are gone. In Core Audio: two streams sample-aligned and two apps receiving identical input, with no re-anchors or breaks. A re-anchor now needs a timeline change lasting 50 ms, so sender stalls no longer move the timeline |
| Phase 3: clock recovered from a received stream | Done (Riedel test pending) | The device clock follows the first received stream, so a sender's drift no longer reaches the playout buffer. In simulation, 8 hours at +/-100 ppm: no re-anchors, no drops, latency constant to +/-1 sample. Over loopback, a 1500 ppm fast sender plays without a gap. In Core Audio, the device rate locked to a sender 200 ppm fast. Next: a 1-hour capture from the Riedel |
| Phase 4: PTP slave and servo | Planned | Rewritten PTP slave driving the media clock |
| Phase 5: transmit on the media clock | In progress | TX is now paced by the media clock and runs whenever a stream is configured, sending silence when no app is playing. Loopback through our own receiver is sample-exact, and TX follows the recovered clock. Still to come: PTP-derived timestamps for Dante and RAVENNA (needs phase 4) and verification in Core Audio |
| Phase 6: resampling for foreign-clock streams | Optional | |
| Spike S3: PTP accuracy against a real grandmaster | Needs hardware | |

## Capabilities

**What has been built and passes synthetic tests:**

- The code compiles on Apple Silicon (arm64) with zero warnings
- The driver installs and loads into coreaudiod without crashing
- The device appears as "AES67 Device" in Audio MIDI Setup
- 128 input + 128 output channels are reported to the system
- RTP receiver: joins multicast, decodes L16/L24, and writes each packet into a buffer indexed by media position, which the IO thread reads lock-free
- RTP transmitter: the IO thread writes the output mix into each TX stream's buffer by media position; the transmitter sends each packet when the media clock reaches its end, continuously (silence when no app is playing), encoding L16/L24
- Core Audio's sample clock is computed by the driver from its own media clock (step 2, phase 1), verified in the real HAL
- IO handler reads/writes Core Audio buffers in the real-time callback
- Received audio is placed by RTP timestamp at a fixed link offset, using kernel arrival timestamps (step 2, phase 2)
- The device clock is recovered from a received stream (step 2, phase 3): a PI servo holds the stream's playout margin constant
- Stream manager handles RX/TX stream lifecycle, channel mapping, and SDP import/export
- Stream configurations load from `streams.json` (see the config search paths below); saving from inside coreaudiod does not work yet (see Known Limitations)
- RT-safe interface boundary prevents accidental mutex access from the audio callback at compile time
- Multicast receiver can bind to a specific network interface (prevents duplicate packets on multi-NIC machines)
- RTP threads are deferred to Core Audio IO lifecycle (zero idle CPU when no client is running)
- An earlier PTP slave implementation exists but is not functional (see Known Limitations); step 2 phase 4 replaces it
- Test sender/receiver tools exercise the network path over loopback
- 13 test suites (SDP parser, channel mapper, ring buffer, RTP receiver, RTP transmitter, PTP clock, stream manager, multi-stream, integration audio path, critical-path regressions, clock foundations, RX timestamp placement, clock recovery), all passing. The clock and receive suites also run clean under ThreadSanitizer, and key tests are checked by mutation (deliberately breaking the code and confirming a test fails)
- IO handler benchmark exists for real-time performance characterisation
- Doxygen API documentation can be generated via `make docs`
- Flexible configuration: supports interface name ("en0") or IP address, auto-detects if not specified
- Multiple config search paths: environment variable, user-level, and system-wide

**What has NOT been tested:**

- TX path (sending audio to AES67 devices) — not verified with real hardware, and not yet interoperable with PTP-aligned receivers (see Known Limitations)
- PTP synchronization with any real network clock source
- Timestamp placement and clock recovery with real AES67 hardware (verified in Core Audio with test senders only)
- Long-term stability under real workloads
- Multi-device synchronisation
- Sample rates beyond 48kHz in practice
- The Manager app controlling live streams

There is a meaningful gap between "paths exercised with test tools" and "works with real audio." This project has not yet crossed the second threshold.

## Known Limitations

### Clocking — The Main Remaining Gap
AES67 depends on every device sharing a PTP-derived media clock. This driver does not do that yet:

- Core Audio's sample clock comes from the driver's own media clock (step 2, phase 1). Without PTP, that clock follows the first received stream (phase 3), or the Mac's host clock when nothing is being received.
- Received audio is placed by RTP timestamp (phase 2), so latency is fixed. Streams are sample-aligned only when their senders share a clock, and the device can follow only one sender's clock. A second sender on an unrelated clock still drifts against it and periodically re-anchors; that needs PTP (phase 4) or resampling (phase 6).
- Transmitted RTP timestamps start at 0 rather than being derived from PTP time, so receivers that align playout to PTP (Dante in AES67 mode, RAVENNA) will not play the stream correctly. TX is paced by the media clock, so it follows the recovered clock and is frequency-locked to the received stream.

Step 2 addresses these; see [Step 2: Clocking Redesign](#step-2-clocking-redesign-in-progress).

### PTP — Existing Code Is Not Functional
The repository contains an earlier PTP implementation (`NetworkEngine/PTP/`) that is not instantiated by the driver. A step 2 assessment found it cannot work as written, so it will be replaced rather than fixed:

- **`PTPSlave`:** it deadlocks on every Sync path (handlers re-lock a mutex they already hold). It compares PTP time (TAI) with UTC, so it can never report lock. It handles `Delay_Resp` on the wrong port, has no frequency estimate, and exposes no mapping from host time to PTP time.
- **`PTPClock` and `PhaseLockedLoop`:** these do not recover a media clock. The PLL is open-loop and the offset sign is inverted.

Step 2 phase 4 writes a new slave and servo, reusing only the socket setup and packet parsing. Multi-device synchronisation should not be relied upon until then.

### Audio Path — Rebuilt Since the Hardware Test
The Riedel test predates step 2. The rebuilt receive path (timestamp placement, clock recovery, IO routing) has been exercised in Core Audio with test senders on the same Mac, but not yet with real AES67 network traffic. The transmit path has only been exercised over loopback. Codec paths (L16/L24) are covered by unit tests but not verified for audible correctness.

### Manager App — Not Connected to the Driver
The SwiftUI Manager app renders its interface but does not control the driver. It writes `~/Library/Application Support/AES67Driver/config.json` in a different schema from the `streams.json` the driver reads, and the driver runs as `_coreaudiod`, so per-user paths never apply. The PTP diagnostics screen shows placeholder data, not measurements. The planned fix is custom HAL properties plus the host's storage API.

### Stream Persistence
The driver reads `streams.json` from `$AES67_CONFIG_PATH`, `~/Library/Application Support/AES67Driver/`, then `/Library/Application Support/AES67Driver/`. The installer creates the system directory owned by root, so the driver (running as `_coreaudiod`) cannot save changes there. Configurations written by an administrator are loaded at startup.

### Other Known Gaps
- TX packets hold sample rate × ptime frames, but the TX stream's own SDP still states 48 frames per packet at other rates.
- The SDP parser truncates fractional `a=ptime` values (0.125, 0.25, 0.333 ms), defaults a missing channel count to 2 rather than 1, and rejects some common `a=ts-refclk` forms.
- The RTP parser ignores CSRC, header extension and padding fields.
- The receive interface address is resolved once at load, so a DHCP renewal or Wi-Fi roam stops receivers until coreaudiod restarts.
- `streams.json` still accepts a `jitterBufferDepth` field, which is now ignored.

## Architecture

```
AES67Driver/
├── Driver/                  # AudioServerPlugIn (libASPL)
│   ├── AES67Device          # Core Audio device declaration
│   ├── AES67IOHandler       # Audio I/O callbacks (lock-free design)
│   ├── PlugInMain           # AudioServerPlugIn entry point
│   └── SDPParser            # SDP file parser (RFC 4566)
├── NetworkEngine/           # Network audio code
│   ├── RTP/
│   │   ├── SimpleRTP        # RTP socket layer (RFC 3550), kernel arrival timestamps
│   │   ├── RTPReceiver      # Receive thread: decode, place, report clock margin
│   │   ├── RtpPlacement     # RTP timestamp -> media position; re-anchor rules
│   │   ├── RxRouting        # Lock-free tables of receive and transmit buffers for the IO thread
│   │   └── RTPTransmitter   # Sends on the media clock from a buffer the IO thread writes
│   ├── Clock/               # Step 2 media clock
│   │   ├── HostTime         # mach tick <-> nanosecond conversion
│   │   ├── MediaClock       # Lock-free host-time <-> media-sample clock
│   │   ├── DeviceTimeline   # Core Audio zero timestamps from the media clock
│   │   ├── TimestampedAudioBuffer  # Audio stored by media position
│   │   ├── ClockServo       # PI servo holding a stream's playout margin constant
│   │   └── RecoveredClockSource    # Reference stream selection; steers MediaClock
│   ├── PTP/                 # Earlier PTP code, not functional (to be replaced)
│   │   └── ptpd/            # Vendored ptpd source (not used)
│   ├── StreamManager        # RX/TX stream lifecycle, IO-gated start/stop
│   └── Discovery/           # SAP stream discovery (RFC 2974)
├── Shared/                  # Common components
│   ├── RingBuffer.hpp       # Lock-free SPSC ring buffer
│   └── Types.h              # Common data structures
├── Tools/                   # Test utilities
│   ├── AES67TestSender      # Sends RTP test packets over loopback
│   ├── AES67TestReceiver    # Receives and validates RTP packets
│   ├── QuickCapture         # Records from a Core Audio device, reports non-silence
│   ├── ClockProbe/          # Spike S2 probe; client measures a device's clock and checks input continuity
│   └── SandboxProbe/        # Spike S1 probe (PTP inside the driver host)
├── Docs/                    # Step 2 plan and spike write-ups
├── ManagerApp/              # SwiftUI configuration app
└── Tests/                   # Unit & integration tests
```

## Code Specifications

These describe what the code is written to target, not what has been verified with real hardware.

| Feature | Code Target | Status |
|---------|-------------|--------|
| Channels | 128 in/out | Reported to system |
| Sample Rates | 44.1kHz - 384kHz | Starts at 48kHz; offers only rates the active streams use; 48kHz verified |
| Bit Depths | L16, L24 | L24 verified with real hardware |
| RTP RX Path | Multicast join, decode, placement by RTP timestamp | Verified in Core Audio with test senders; earlier design verified with Riedel Artist |
| RTP TX Path | Encode, multicast send on the media clock | Continuous; loopback sample-exact; follows the recovered clock; not yet verified in Core Audio or with hardware |
| Playout Latency | Fixed link offset, 8 x packet time (8 ms) | Implemented; reported to Core Audio as input latency |
| Multicast Binding | Interface-specific via IP_MULTICAST_IF | **Verified working on multi-NIC** |
| IO Lifecycle | RTP threads start/stop with Core Audio IO | Implemented, verified in DAW |
| RT-Safe Boundary | Compile-time separation of RT/non-RT paths | Implemented |
| Device Clock | Zero timestamps from the driver's media clock | **Implemented (step 2 phase 1)**, verified in the real HAL |
| Media Clock Recovery | From a received stream, later from PTP | **From a received stream: implemented (step 2 phase 3)**, verified in the real HAL with a test sender. From PTP: planned (phase 4) |
| PTP Network Sync | IEEE 1588 slave-only | Planned rewrite (step 2 phase 4); earlier code not functional |
| Stream Persistence | JSON config in /Library/Application Support/ | Loads at startup; saving from coreaudiod not working |
| Interface Config | Name ("en0"), IP, or auto-detect | **Implemented** |
| Driver Transport | AudioServerPlugIn | Loads into coreaudiod |

## Building

### Prerequisites

```bash
brew install cmake

# Install libASPL (AudioServerPlugIn framework)
brew tap gavv/gavv
brew install libaspl
```

### Build & Install

```bash
git clone https://github.com/maxajbarlow/AES67_macos_Driver.git
cd AES67_macos_Driver

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j

# Run tests
ctest --output-on-failure

# Generate API docs (requires doxygen)
make docs

# Install the driver
sudo cp -R AES67Driver.driver /Library/Audio/Plug-Ins/HAL/

# Restart Core Audio to load the driver
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod

# Verify it appears
system_profiler SPAudioDataType | grep -A 5 "AES67"
```

### Checking the Device Clock

`AES67ClockProbeClient` runs IO on a device and measures what Core Audio does with its clock: rate against an expected value, timeline continuity and overloads. It is built with the investigation spikes:

```bash
cmake .. -DBUILD_SPIKES=ON && make AES67ClockProbeClient
./Tools/AES67ClockProbeClient --uid com.aes67.driver.device --seconds 25 --constant 0 --no-ramp
```

With `--sawtooth <period> <amplitude>` it also checks that input channel 0 carries an unbroken test sawtooth. `--compare <a> <b>` counts frames where two channels differ, and `--dump <file>` writes per-cycle checksums so two clients' input can be compared.

### Build Manager App

```bash
cd ManagerApp
./build.sh
open AES67Manager.app
```

## Why No Kernel Extension Required

This driver uses Apple's AudioServerPlugIn architecture:

- Runs entirely in user space within coreaudiod
- No SIP changes or "Reduced Security" boot mode required
- Standard file copy installation
- Apple-supported approach for modern macOS

## Help Wanted

This project needs real-world testing before any audio claims can be made. If you have access to:

- AES67 network audio devices
- Dante-enabled equipment
- RAVENNA systems
- Professional audio software (Logic Pro, Pro Tools, etc.)

Please try building and testing. Open GitHub issues with detailed results — even "it didn't work" reports are valuable.

## Contributing

Contributions welcome, especially:

- **Hardware testing reports** (most needed)
- Bug fixes with reproduction steps
- Access to a PTP grandmaster for spike S3 (measuring PTP accuracy from inside the driver host), ahead of the step 2 phase 4 PTP rewrite
- Testing multicast interface binding on multi-NIC setups
- DAW compatibility testing (Logic Pro, Pro Tools, Ableton, etc.)

### Guidelines

- C++17 standard
- Maintain lock-free audio thread safety
- Add unit tests for new code

## License

GPL-3.0 - See LICENSE file.

### Dependencies

- **libASPL**: MIT License - AudioServerPlugIn framework

## Acknowledgments

- [libASPL](https://github.com/gavv/libASPL) - Modern C++ AudioServerPlugIn framework
- AES67-2018 specification
- RFC 3550 (RTP), RFC 4566 (SDP), RFC 2974 (SAP)

---

*This is experimental software. The driver compiles, loads and passes its tests, and its rebuilt receive path and recovered clock are verified in Core Audio with test senders. An earlier RX path received audio from real AES67 hardware in short tests. Long sessions with real hardware, TX interoperability and multi-device synchronisation have not been verified.*
