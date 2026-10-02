# Step 2: Clocking Redesign Plan

Status: draft for review (October 2026)

## Why

AES67 works because every device shares one media clock derived from PTP. Today the driver runs four independent clocks:

| Clock | Drives | Source today |
|---|---|---|
| Core Audio device clock | When Core Audio reads and writes the device | libASPL default `GetZeroTimeStampImpl`: the Mac's host clock at the nominal rate |
| RX consume thread | When received packets move into the ring buffers | `steady_clock` timer, now steered to the sender's packet rate (step 1) |
| TX thread | When packets are sent | `steady_clock` timer at a fixed 1 ms |
| Remote sender | When packets arrive | Its PTP-locked media clock |

Consequences: sender-versus-Mac drift accumulates in the ring buffers until they under- or overflow; received RTP timestamps are ignored, so latency is not fixed and streams are not sample-aligned with each other; transmitted RTP timestamps start at 0, so PTP-aligned receivers (Dante in AES67 mode, RAVENNA) cannot play them; two apps recording at once drain each other's input.

## Goal

One media clock for the whole device. Core Audio's sample clock, RX placement and TX timestamps all derive from it. When PTP is available the media clock is PTP; when it is not, the device follows a received stream or, last resort, the host clock.

### Success criteria

1. Simulated 8-hour run with a sender at +/-100 ppm: zero underruns or overruns, latency constant to +/-1 sample.
2. Two streams from senders on the same PTP grandmaster arrive sample-aligned in Core Audio.
3. A 1-hour RX capture from real hardware (Riedel Artist) shows no discontinuities in a test tone.
4. A Dante or RAVENNA receiver plays the driver's TX stream without dropouts.
5. Two apps recording simultaneously receive identical input.

## Verified facts this plan rests on

- **Kernel receive timestamps are already in Core Audio's time base.** `SO_TIMESTAMP_MONOTONIC` returns `mach_absolute_time` ticks (verified on this Mac: each timestamp fell between `mach_absolute_time` reads taken before the send and after the receive, 10-20 us ahead of userspace). PTP event timestamps can therefore be used directly against Core Audio host time, with no clock translation.
- **Host ticks are not nanoseconds.** `mach_timebase_info` is 125/3 on Apple Silicon. Nothing in the repo uses it today.
- **Core Audio follows a model-driven device clock exactly** (spike S2).
- **Zero timestamps map sample time to `mach_absolute_time`** (AudioServerPlugIn.h). The period must be at least 10923 frames. Changing the seed tells the HAL the timeline restarted. `kAudioDeviceClockAlgorithmRaw` disables HAL filtering, appropriate when timestamps come from a servo model.
- **Nothing on this Mac currently owns UDP 319/320, and the sandboxed driver host can use them** (spike S1).
- **The existing PTP, PLL and resampler code is not a foundation** (separate assessment): `PTPSlave` deadlocks on every Follow_Up and Sync path, compares TAI to UTC so it can never lock, has no frequency estimate and no host-time mapping; `PhaseLockedLoop` is open-loop; the resampler resets phase every block. Only `PTPSlave`'s socket setup, packet field parsing and Delay_Req builder are worth carrying forward.

## Architecture

```
                        +-----------------------------+
   PTP (319/320) -----> |  ClockSource (one active)    |
   RX RTP timestamps -> |   PTP | StreamRecovered | Host|
                        +--------------+--------------+
                                       | publishes model (seqlock)
                                       v
                        +-----------------------------+
                        |  MediaClock                  |
                        |  host ticks <-> media sample |
                        +---+-------------+-----------+
                            |             |
        GetZeroTimeStampImpl|             | toHost(M), toMedia(h)
                            v             v
  Core Audio IO --> device sample T --> media sample M = T + mediaBase
        |                                          ^
        | read [M - linkOffset, +n)                | write at RTP-derived M
        v                                          |
  +-----------------------------+      +---------------------------+
  | PlayoutBuffer (per RX stream)| <--- | Receive thread (decode)   |
  | slots tagged by media sample |      | no consume thread         |
  +-----------------------------+      +---------------------------+
        | write mix at M                     
        v                                    
  +-----------------------------+      +---------------------------+
  | TxBuffer (per TX stream)    | ---> | TX thread: wakes at        |
  | slots tagged by media sample|      | toHost(M + F), sends RTP   |
  +-----------------------------+      | ts = M + mediaclk offset   |
                                       +---------------------------+
```

### MediaClock

An affine map between host time and media position, published lock-free so the IO thread can read it:

```
media(h) = anchorMedia + (h - anchorHost) * samplesPerTick
host(M)  = anchorHost  + (M - anchorMedia) / samplesPerTick
samplesPerTick = fs * rateRatio * timebase.numer / (timebase.denom * 1e9)
```

- With PTP, media position is PTP time in samples since the PTP epoch (`ptpNs * fs / 1e9`), so every device on the grandmaster agrees on it.
- Updated by the active clock source a few times per second; readers extrapolate between updates. Rate changes are smooth (servo-filtered); a phase step bumps the generation, which becomes the HAL seed.
- Publication via a seqlock or double-buffered snapshot; readers never block.

### Device clock

Override `GetZeroTimeStampImpl`:

- At `StartIO`, choose `mediaBase` (a multiple of the period near the current media position), so device sample time `T = M - mediaBase` starts near 0.
- Zero timestamp: `k = floor((media(now) - mediaBase) / P)`, return `sampleTime = k * P`, `hostTime = host(mediaBase + k * P)`, `seed = generation`.
- `GetClockAlgorithm` returns Raw; `GetClockIsStable` reflects lock state; `GetClockDomain` returns a non-zero ID derived from the grandmaster identity and domain, so other devices on the same PTP domain are reported as synchronised.
- Sample rate changes rebase `mediaBase`.

### RX: playout by timestamp

- RTP timestamp to media position: unwrap `(rtpTs - mediaclkOffset) mod 2^32` to the value nearest `media(now)` (the `a=mediaclk:direct=` offset from the SDP, per stream).
- The receive thread decodes each packet straight into a per-stream `PlayoutBuffer` at its media position, tagging the slot with that position. No jitter buffer, consume thread, prefill, rate controller or resync logic.
- The IO handler reads each mapped channel at `M - linkOffset` for the requested range. A slot whose tag does not match the requested position reads as silence. Loss, outage and restart handling become structural: a missing packet's slot is stale, so it plays as silence; a restarted sender's timestamps place it correctly; latency cannot grow.
- Reads are non-destructive, so any number of clients read identical input.
- Counters: late (arrived after playout), early (beyond the buffer horizon, signals clock mismatch), missing.
- Keep the step 1 SSRC gate so a second sender on the group cannot interleave.
- `linkOffset` is configurable and reported to the HAL as input latency.

### TX: timestamps from the media clock

- `OnWriteMixedOutput` writes the mix into each TX stream's `TxBuffer` at `M = T + mediaBase`.
- The TX thread wakes at `host(M + F)` for each packet of F frames (from ptime and the current rate, fixing the 48 kHz-only limitation), reads `[M, M + F)`, and sends RTP with `ts = (M + mediaclkOffset) mod 2^32`. Slots not written (no client running) send silence.
- Packets leave just after their last sample's media time, as hardware senders do, so they are neither early nor late at receivers.
- Generated SDP gains correct `a=ts-refclk` and `a=mediaclk:direct=` lines.

### Clock sources

| Source | When used | Accuracy |
|---|---|---|
| PTP | Locked to a grandmaster on the configured domain | Frequency to well under 1 ppm; phase limited by software timestamping (measure in spike S3; expect tens of microseconds) |
| StreamRecovered | No PTP, at least one RX stream: fit RTP timestamps against arrival host time for a reference stream | Drift-free relative to that sender; arrival jitter averaged out |
| Host | Nothing else available | Free-running, today's behaviour |

Selection is automatic with a configurable preference, and switching sources is a phase step (new seed).

### PTP slave (new)

#### Target network (from the Riedel Artist SIC AES67 card's PTP settings, October 2026)

| Setting | Value | What the slave must do |
|---|---|---|
| PTP mode | Hybrid | Sync, Follow_Up and Announce arrive by multicast; **Delay_Req is sent unicast to the master's address** (the source of its Sync/Announce), and Delay_Resp arrives unicast on port 320. Keep multicast Delay_Req as an option for non-hybrid networks. |
| Role | TimeReceiver (slave) | The Riedel is not the grandmaster; a separate device on the network is. |
| Domain | 0 | The default. |
| Announce interval / receipt timeout | 1 (2 s) / 3 | Derive the timeout from the received `logAnnounceInterval` (here 6 s), never a fixed value. |
| Sync interval / delay request interval | 0 / 0 (1 per second) | These are the Riedel's own port settings; the grandmaster sets the real rates, which spike S3 must measure. If Sync really is 1 Hz, the servo gets one measurement per second and needs a longer averaging window than an 8 Hz network. |
| Media 1 / Media 2 | Shared PTP settings | Suggests dual networks (SMPTE ST 2022-7). The driver uses one network; 2022-7 redundancy is out of scope for step 2. |

From a SmartPanel's AES67 tab (RSP-1232HL):

| Setting | Value | Implication |
|---|---|---|
| Packet time | 1.000 ms | 48 frames at 48 kHz, matching the Riedel SDP in `Docs/Examples/`. |
| Receive buffer | 8.000 ms (8 x packet time) | The network's receive latency convention; basis for the default link offset (decision 1). Also the arrival window our TX packets must meet in phase 5. |
| Play mode | synton | Unconfirmed reading: syntonised playout (frequency-locked, latency set by the buffer) rather than absolute time alignment. If so, TX interop needs the right rate more than PTP phase, which phase 3's recovered clock already provides. Check Riedel's documentation. |
| Media 1 address | 10.46.70.211, port 6060 | Same address range as the development Mac, but during spike S1 that Mac heard no PTP on 224.0.1.129, so S3 needs a port on the PTP network. |

The card also has an NMOS tab, so streams there may be managed through NMOS (IS-04/IS-05) rather than SAP or SDP files. That is a candidate for after step 2.

- Reuse from `PTPSlave`: socket setup (plus `SO_TIMESTAMP_MONOTONIC`), header and Announce field parsing, Delay_Req builder, requesting-port matching.
- Write fresh:
  - Delay_Resp on the general port.
  - Correction field subtracted correctly.
  - Sync and Follow_Up filtered by the selected master's port identity.
  - Full BMCA dataset comparison with foreign-master qualification.
  - Announce timeout from the received interval.
  - No lock held across calls.
- Servo: (host ticks, PTP ns) pairs into a windowed least-squares fit or a PI loop with a frequency integrator, with minimum-delay outlier rejection (software timestamps are positively skewed). It outputs the MediaClock model directly.
- t3 (Delay_Req send time) is taken in userspace just after `sendto`; this asymmetry biases phase slightly, and a calibration offset can absorb it.
- Holdover on master loss keeps the last rate; re-acquisition slews if the error is small, otherwise steps (new seed).
- Diagnostics snapshot (state, master, offset, rate, path delay, counters) for the Manager app once the control path exists.

### ASRC (optional, last)

Only needed for streams not on the device's clock: a different PTP domain, or several unsynchronised senders without PTP. Use an established variable-ratio library rather than the existing linear resampler:
- **libsamplerate** (BSD-2): smooth ratio changes, no allocation after setup.
- **soxr** (LGPL): lower latency.

The repo is GPL-3.0, so GPL options such as zita-resampler are also acceptable. Run the ASRC on the receive thread, never the IO thread, and drive its ratio from the playout buffer's position error.

## What this replaces

The step 1 receive pacing machinery (consume thread, `LockFreeCircularJitterBuffer`, `RateController`, prefill, resync and underrun credit) is superseded by timestamp placement. Step 1 was still necessary to make the current design behave; step 2 makes those behaviours structural. The step 1 regression tests keep their intent (loss plays as silence, restarts recover, outages do not add latency, second senders are ignored) and are rewritten against the new design.

Also removed: `PTPClock`, `PTPDInterface`, `PhaseLockedLoop`, `CustomSys`, `NetworkEngine/Resampling/*`, `Tests/TestPTPClock.cpp`, and the vendored ptpd source (kept "for reference" but never built).

## Phases

Each phase is a separate PR, test-first, and leaves the driver working.

### Phase 0: Foundations and spikes (done)

- `NetworkEngine/Clock/`: `HostTime.h` (mach timebase conversions), `MediaClock.h` (seqlock-published model, whole sample plus fraction for sub-sample precision at PTP-epoch scale), and `TimestampedAudioBuffer` (per-frame tags used as seqlocks). The PlayoutBuffer and TxBuffer in this plan are both instances of `TimestampedAudioBuffer`.
- Covered by `Tests/TestClockFoundations.cpp`. Its stress tests fail against deliberately broken seqlocks (verified by mutation), and the suite is clean under ThreadSanitizer.
- Deterministic unit tests with virtual time:
  - Mapping round-trips.
  - Wraparound.
  - Late and early writes.
  - Concurrent writer with multiple readers.
- Spikes S1-S3 below, before Phase 1 commits to an approach.
- No behaviour change.

### Phase 1: Device clock from MediaClock (done)

- `GetZeroTimeStampImpl` override with the Host source, Raw algorithm, seed handling and rate-change rebase.
- Exit: zero timestamps monotonic and period-aligned in tests; in the real HAL, measured device rate equals nominal and audio is unchanged.
- Implementation:
  - The `mediaBase` idea became a timeline **origin** stored in the `MediaClock` snapshot, set by `reset` and kept by `setRate` and `step`. Device sample time is `T = M - origin`, published atomically with the rest of the clock, so it cannot race. `NetworkEngine/Clock/DeviceTimeline.h` computes zero timestamps as a pure function of a snapshot.
  - The device restarts its timeline (new seed) at IO start and on sample rate changes.
- Result:
  - **Tests:** `TestClockFoundations` covers timestamp alignment, bracketing, exact host mapping and monotonicity across a 10 s sweep. `TestCriticalPathRegressions` checks the device's timestamps land exactly on its own MediaClock, which libASPL's default clock would not.
  - **Real HAL:** with the installed driver, the client (`AES67ClockProbeClient --uid com.aes67.driver.device --constant 0 --no-ramp --expect-level 0.25`) measured the rate at +0.0 ppm (HAL actual rate 48000.000 Hz), with 0 timeline jumps and 0 overloads in 2345 callbacks.
  - **Receive audio:** host-only test packets through a receive-only config arrived unbroken: 100.0% of 1,200,640 input samples were at the expected level.

### Phase 2: RX by timestamp

- Receive thread decodes into PlayoutBuffers; IO handler reads by sample time with link offset; consume thread and jitter buffer deleted; link offset reported as latency.
- Add a tone-continuity analyser tool (extend `QuickCapture`) for long hardware captures.
- Exit: simulated two-stream alignment exact; loss, restart, outage and second-sender tests pass against the new design; two-client reads identical.

### Phase 3: Stream-recovered clock

- Device clock follows a reference RX stream's RTP rate.
- First user-visible payoff: drift-free RX from a single sender (the Riedel) without PTP.
- Exit: success criterion 1 in simulation; criterion 3 on hardware.

### Phase 4: PTP

- New slave and servo as the PTP clock source, source selection, clock domain, diagnostics.
- Tests:
  - Golden-vector message parsing.
  - Servo simulation with jittered, skewed timestamps.
  - Loopback against a scripted PTP master.
  - Hardware run against the Riedel or a Dante device as grandmaster.
- Exit: frequency lock within about 30 s and phase settled within about 2 minutes at a 1 Hz Sync rate (faster on faster networks); hybrid-mode unicast Delay_Req verified against the target network; criterion 2.

### Phase 5: TX on the media clock

- TxBuffer, media-paced sender, RTP timestamps from media time, ptime and framecount from the rate, compliant SDP.
- Exit: TX to RX loopback sample-exact; criterion 4 with a Dante or RAVENNA receiver.

### Phase 6 (optional): ASRC for foreign-clock streams

## Spikes (do first)

- **S1, PTP inside the sandbox: resolved, PTP runs in-process.** Inside the driver host (`_coreaudiod`, macOS 26.0.1) the probe bound UDP 319/320, joined 224.0.1.129, sent, and received with kernel monotonic timestamps, with or without `AudioServerPlugIn_Network`. Host threads were scheduled up to 31.7 ms late, so kernel timestamps are mandatory. Details: [Spikes/S1-PTP-Sandbox.md](Spikes/S1-PTP-Sandbox.md).
- **S2, HAL acceptance: resolved, Core Audio follows the model exactly.** With the Raw algorithm and a 16384-frame period, the measured rate matched a +300 ppm model to 0.1 ppm, tracked a +300 to -300 ppm ramp within 3 ppm (re-anchoring every 100 ms), and a phase step with a new seed produced exactly one clean timeline jump with no overloads in 3284 IO cycles. Core Audio adds no smoothing under Raw, so the servo must publish a smooth rate and slew small phase errors rather than step. Details: [Spikes/S2-HAL-Clock.md](Spikes/S2-HAL-Clock.md).
- **S3, PTP accuracy.** Log offset and path-delay statistics against a real grandmaster to set link offset defaults and lock thresholds. On the target network this means the network's grandmaster, not the Riedel (a TimeReceiver). Also measure the actual Sync, Announce and Delay_Resp rates, and confirm the master answers unicast Delay_Req (hybrid mode).

## Risks

- **Software timestamp asymmetry biases phase.** Mitigation: minimum-delay filtering, a calibration offset, and a generous default link offset.
- **Other PTP software on the same Mac** (another AoIP driver) contends for ports 319/320. Mitigation: `SO_REUSEPORT` and a documented limitation.
- **Sample rate changes mid-stream.** Mitigation: rebase `mediaBase`, flush buffers, bump the seed.
- **Scope.** This is the largest change in the project. Mitigation: phased PRs, each independently shippable, with deterministic virtual-time tests so timing does not make the suite flaky.

## Decisions needed

1. **Default link offset: decided, 8 x packet time (8 ms at 1 ms packets), configurable 0.25-20 ms.** Matches the target network's receive buffer convention, favouring robustness on a busy network; revisit after spike S3 measures jitter and PTP accuracy.
2. **Stream-recovered mode (Phase 3).** Recommended, because it delivers drift-free RX from the Riedel before PTP lands.
3. **PTP placement.** Resolved by spike S1: in-process.
4. **ASRC.** Defer to Phase 6, choose the library then.
5. **Tracking.** Whether to track phases as GitHub issues under a milestone.
