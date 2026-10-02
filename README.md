# AES67 macOS Audio Driver

> **EXPERIMENTAL SOFTWARE — USE WITH CAUTION**
>
> This is a work-in-progress open-source AES67 audio driver for macOS. The RX (receive) path has been **verified working with real AES67 hardware** (Riedel Artist intercom system) and audio successfully flows into DAW software (Reaper). That verification used short captures and predates the October 2026 critical-path fixes (see [Recent Fixes](#recent-fixes)); it needs repeating over longer sessions. The TX (transmit) path has not yet been tested with real hardware.
>
> **Production use is not recommended without thorough testing in your environment.** This project is under active development.

A work-in-progress open-source virtual audio driver for macOS that aims to provide AES67 network audio support. Built as a user-space AudioServerPlugIn using the libASPL framework.

## Current Status

**Verified Working (Real Hardware):**

- **RX Path Tested with Riedel Artist:** Audio successfully received from Riedel Artist intercom system via AES67 multicast and recorded in Reaper
- Multicast interface binding verified on multi-NIC machine (correctly binds to specified interface)
- L24 encoding at 48kHz, 1ms packet time verified working with professional broadcast hardware

## Recent Fixes

A full technical review in October 2026 found several defects on the critical audio path. These are fixed, each with a regression test in `Tests/TestCriticalPathRegressions.cpp` that failed before the fix:

- **TX sent only silence.** libASPL delivers mixed output through `OnWriteMixedOutput` (`DeviceParameters::EnableMixing` defaults to true); the driver only implemented `OnWriteClientOutput`, which is never called in that mode.
- **The device ran at 44.1 kHz.** libASPL's default nominal rate is 44.1 kHz while the streams declared 48 kHz, and rate changes from Audio MIDI Setup never reached the driver. The device now starts at 48 kHz, offers only rates the active streams can deliver, and applies rate changes to the driver, StreamManager and both streams' formats.
- **The RX pacing loop ran away.** It measured the device ring buffer, which the consume thread cannot regulate, and ran itself to its +0.5% clamp within about 10 seconds, causing periodic underruns and steadily growing latency. It now holds the jitter buffer at its prefill depth, which locks consumption to the sender's packet rate.
- **A sender restart caused up to 65 seconds of silence.** The receiver now follows a new source once 4 consecutive packets agree, and ignores single late packets and second senders on the same group.
- **Lost packets shifted the timeline.** A lost packet now plays as one packet of silence, so streams stay sample-aligned with each other.
- **Saved TX streams reloaded as RX streams**, and stale jitter buffer slots could block newer packets.
- The plug-in now declares `AudioServerPlugIn_Network`, which Apple requires for network access from the sandboxed audio host.

## Capabilities

**What has been built and passes synthetic tests:**

- The code compiles on Apple Silicon (arm64) with zero warnings
- The driver installs and loads into coreaudiod without crashing
- The device appears as "AES67 Device" in Audio MIDI Setup
- 128 input + 128 output channels are reported to the system
- RTP receiver: joins multicast, decodes L16/L24, writes to ring buffers
- RTP transmitter: reads from ring buffers, encodes L16/L24, sends multicast
- Lock-free SPSC ring buffers bridge network and Core Audio IO threads
- IO handler reads/writes Core Audio buffers in the real-time callback
- Lock-free jitter buffer absorbs network timing variation (configurable depth, 32–4096 slots)
- Stream manager handles RX/TX stream lifecycle, channel mapping, and SDP import/export
- Stream configurations load from `streams.json` (see the config search paths below); saving from inside coreaudiod does not work yet (see Known Limitations)
- RT-safe interface boundary prevents accidental mutex access from the audio callback at compile time
- Multicast receiver can bind to a specific network interface (prevents duplicate packets on multi-NIC machines)
- RTP threads are deferred to Core Audio IO lifecycle (zero idle CPU when no client is running)
- PTP slave-only implementation written (IEEE 1588 message exchange, offset/delay calculation, lock detection), but not yet wired into the driver
- Test sender/receiver tools exercise the network path over loopback
- 10 test suites (SDP parser, channel mapper, ring buffer, RTP receiver, RTP transmitter, PTP clock, stream manager, multi-stream, integration audio path, critical-path regressions); one integration check fails because of a known bug in the test itself
- IO handler benchmark exists for real-time performance characterisation
- Doxygen API documentation can be generated via `make docs`
- Flexible configuration: supports interface name ("en0") or IP address, auto-detects if not specified
- Multiple config search paths: environment variable, user-level, and system-wide

**What has NOT been tested:**

- TX path (sending audio to AES67 devices) — not verified with real hardware, and not yet interoperable with PTP-aligned receivers (see Known Limitations)
- PTP synchronization with any real network clock source — the PTP slave code has been written but never run against a real grandmaster
- The configurable jitter buffer under varied network jitter conditions
- Long-term stability under real workloads
- Multi-device synchronisation
- Sample rates beyond 48kHz in practice
- The Manager app controlling live streams

There is a meaningful gap between "paths exercised with test tools" and "works with real audio." This project has not yet crossed the second threshold.

## Known Limitations

### Clocking — The Main Remaining Gap
AES67 depends on every device sharing a PTP-derived media clock. This driver does not do that yet:

- Core Audio's sample clock is the Mac's host clock, not the network's PTP clock.
- RTP timestamps are not used to place received audio, so latency is not fixed and streams from different senders are not sample-aligned with each other.
- Transmitted RTP timestamps start at 0 rather than being derived from PTP time, so receivers that align playout to PTP (Dante in AES67 mode, RAVENNA) will not play the stream correctly.
- Drift between a sender's clock and the Mac's clock accumulates in the device ring buffer. Over a long session this eventually causes an underrun or overrun. Fixing it needs either the device clock locked to the PTP media clock or asynchronous sample rate conversion.

This is the next major piece of work.

### PTP — Code Written, Not Wired In
The PTP subsystem has two layers:

- **Media clock recovery (implemented):** `PTPClock` correlates RTP timestamps with local time per AES67-2018 Section 8.2. A Phase-Locked Loop tracks clock drift between the remote source and local audio hardware. Reference point history enables drift ratio calculation for adaptive resampling. In local-clock fallback mode, this is sufficient for single-device operation — audio can flow through the driver using local timing.

- **Network PTP synchronisation (code written, untested):** `PTPSlave` implements IEEE 1588 slave-only mode — Sync/Follow_Up/Delay_Req/Delay_Resp message exchange, offset and path delay calculation, 8-sample moving average filtering, lock detection with hysteresis, and frequency drift estimation. It joins the 224.0.1.129 multicast group on ports 319/320 and feeds measurements into the existing PLL via `PTPDInterface`. However, this code is **not instantiated by the driver**, has **never been tested against a real PTP grandmaster**, and has known bugs: `Delay_Resp` is handled on the event socket (port 319) instead of the general socket (320), so path delay is never measured, and fixing that naively would deadlock `handleDelayResp`. Multi-device synchronisation should not be relied upon.

### Audio Path — Exercised Synthetically Only
The RTP receiver/transmitter, jitter buffer, IO handler, and ring buffers have been exercised with test sender/receiver tools over loopback, but never with real audio content or real AES67 network traffic. Codec paths (L16/L24) are covered by unit tests but not verified for audible correctness.

### Manager App — Not Connected to the Driver
The SwiftUI Manager app renders its interface but does not control the driver. It writes `~/Library/Application Support/AES67Driver/config.json` in a different schema from the `streams.json` the driver reads, and the driver runs as `_coreaudiod`, so per-user paths never apply. The PTP diagnostics screen shows placeholder data, not measurements. The planned fix is custom HAL properties plus the host's storage API.

### Stream Persistence
The driver reads `streams.json` from `$AES67_CONFIG_PATH`, `~/Library/Application Support/AES67Driver/`, then `/Library/Application Support/AES67Driver/`. The installer creates the system directory owned by root, so the driver (running as `_coreaudiod`) cannot save changes there. Configurations written by an administrator are loaded at startup.

### Other Known Gaps
- TX always sends 48 frames per 1 ms packet, so it is only correct at 48 kHz.
- The SDP parser truncates fractional `a=ptime` values (0.125, 0.25, 0.333 ms), defaults a missing channel count to 2 rather than 1, and rejects some common `a=ts-refclk` forms.
- The RTP parser ignores CSRC, header extension and padding fields.

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
│   │   ├── SimpleRTP        # RTP socket layer (RFC 3550)
│   │   ├── RTPReceiver      # Packet receive + decode
│   │   ├── RateController   # Paces RX consumption to the sender's rate
│   │   ├── RTPTransmitter   # Packet encode + send
│   │   └── LockFreeCircularJitterBuffer
│   ├── PTP/
│   │   ├── PTPClock         # Media clock recovery (AES67 Section 8.2)
│   │   ├── PTPSlave         # IEEE 1588 slave-only (written, untested)
│   │   ├── PhaseLockedLoop  # Audio clock drift tracking
│   │   ├── PTPDInterface    # PTP interface (stub fallback available)
│   │   └── vendor/ptpd/     # Vendored ptpd source (not used)
│   ├── StreamManager        # RX/TX stream lifecycle, IO-gated start/stop
│   ├── Resampling/          # Sample rate conversion
│   └── Discovery/           # SAP stream discovery (RFC 2974)
├── Shared/                  # Common components
│   ├── RingBuffer.hpp       # Lock-free SPSC ring buffer
│   └── Types.h              # Common data structures
├── Tools/                   # Test utilities
│   ├── AES67TestSender      # Sends RTP test packets over loopback
│   └── AES67TestReceiver    # Receives and validates RTP packets
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
| RTP RX Path | Multicast join, decode, jitter buffer | **Verified with Riedel Artist** |
| RTP TX Path | Encode, multicast send | Audio path fixed Oct 2026; 48kHz only; not hardware-verified |
| Jitter Buffer | Configurable 32–4096 slots, lock-free | Default 256; consumption paced to sender rate |
| Multicast Binding | Interface-specific via IP_MULTICAST_IF | **Verified working on multi-NIC** |
| IO Lifecycle | RTP threads start/stop with Core Audio IO | Implemented, verified in DAW |
| RT-Safe Boundary | Compile-time separation of RT/non-RT paths | Implemented |
| Media Clock Recovery | RTP↔time correlation, PLL, drift tracking | Code exists, not wired into the driver |
| PTP Network Sync | IEEE 1588 slave-only (PTPSlave) | Code written, not wired in, known bugs |
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
- Locking the device clock to the PTP media clock (see Known Limitations)
- Testing PTPSlave against a real IEEE 1588 grandmaster
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

*This is experimental software. The driver compiles, loads and passes synthetic tests, and the RX path has received audio from real AES67 hardware in short tests. Long-session stability, TX interoperability and multi-device synchronisation have not been verified.*
