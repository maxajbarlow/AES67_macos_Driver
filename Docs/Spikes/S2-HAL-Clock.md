# Spike S2: Does Core Audio follow a model-driven device clock?

Date: 2 October 2026. macOS 26.0.1, Apple Silicon (arm64).
Plan: [Step2-Clocking-Plan.md](../Step2-Clocking-Plan.md).

## Question

Step 2 makes the device's clock a host-time-to-sample-time model that the PTP servo (or a recovered stream clock) keeps updating. Before building on that, check:

- Will Core Audio accept zero timestamps computed from such a model with the Raw clock algorithm and a custom period?
- Will it follow the model's rate as the rate changes?
- Will it resynchronise cleanly when the model steps and changes the seed?

## Method

`Tools/ClockProbe/` (CMake option `BUILD_SPIKES=ON`) builds two things:

- **`AES67ClockProbe.driver`:** a throwaway plug-in with one input-only device, "AES67 S2 Clock Probe". The device cannot be the default or system device. It overrides `GetZeroTimeStampImpl` with an affine clock model published through a seqlock. A schedule thread re-anchors the model every 100 ms at the current position, the way the real servo will. Settings:
  - Zero timestamp period: 16384 frames (HAL minimum 10923).
  - Clock algorithm: Raw.
  - Schedule while IO runs: +300 ppm for 15 s; a +4800-sample phase step with a new seed at 15 s; a linear ramp to -300 ppm over the next 15 s; then -300 ppm.
  - Its input carries a ramp derived from the sample time, so dropped or repeated samples are visible.
- **`AES67ClockProbeClient`:** runs an IOProc on the device for 35 s, records every callback's sample time and host time, fits the rate in 3 s windows, and checks timeline and input continuity. It also polls `kAudioDevicePropertyActualSampleRate` and counts `kAudioDeviceProcessorOverload` notifications.

To reproduce: build both targets, copy the driver to `/Library/Audio/Plug-Ins/HAL/`, restart coreaudiod, and run the client. It prints PASS or FAIL.

## Results

```
Rate in 3 s windows (measured vs scheduled, ppm relative to 48000 Hz):
    1.0-  4.0 s  measured   +300.0  scheduled   +300.0  error   +0.0
    4.0-  7.0 s  measured   +300.0  scheduled   +300.0  error   +0.0
    7.0- 10.0 s  measured   +300.0  scheduled   +300.0  error   -0.0
   10.0- 13.0 s  measured   +300.0  scheduled   +300.0  error   -0.0
   16.0- 19.0 s  measured   +202.2  scheduled   +200.0  error   +2.2
   19.0- 22.0 s  measured    +82.1  scheduled    +80.0  error   +2.1
   22.0- 25.0 s  measured    -38.2  scheduled    -40.0  error   +1.8
   25.0- 28.0 s  measured   -158.3  scheduled   -160.0  error   +1.7
   28.0- 31.0 s  measured   -277.1  scheduled   -280.0  error   +2.9
   31.0- 34.0 s  measured   -300.0  scheduled   -300.0  error   -0.0
  timeline jump at t=15.004 s: sample time advanced 5324.0 for 512 frames

callbacks 3284, frames/callback 512, timeline jumps 1, ramp breaks 1, overloads 0
PASS
```

- **Rate followed exactly.** At a constant offset the measured rate matched to 0.1 ppm, and `kAudioDevicePropertyActualSampleRate` read 48014.401 Hz (+300.0 ppm). Core Audio applied no smoothing of its own with the Raw algorithm.
- **The ramp tracked within 3 ppm.** The consistent +2 ppm bias comes from the probe, not Core Audio: it updates the rate in 100 ms steps, so on a 40 ppm/s ramp it lags by half a step, 40 x 0.05 = 2 ppm.
- **The phase step resynchronised cleanly.** There was exactly one timeline jump and one input discontinuity, both at the step: +4812 samples beyond the normal 512, which is the 4800 step plus the +300 ppm advance over the update interval. IO continued without interruption. Core Audio's rate estimate reset at the seed change (48000.000 Hz at 15 s) and then followed the ramp.
- **No overloads in 3284 IO cycles.** The probe's `GetZeroTimeStamp` was called 3285 times, about once per cycle.
- **One unattributed log message.** During the IO window coreaudiod logged a burst of three "HALS_PlugIn::HostInterface_PropertiesChanged: the object is not valid" errors, one second after IO started. The same message appears repeatedly from other plug-ins while coreaudiod restarts, so it could not be attributed to the probe, and it had no effect on IO.

## Conclusions

1. **The plan's device clock design works as specified.** Raw algorithm, a period above the minimum, an affine model published lock-free and re-anchored every 100 ms, and a seed change on a phase step.
2. **Core Audio does no extra filtering under Raw, so the servo must do all the smoothing.** Whatever rate the model reports is what Core Audio runs at, so the servo must never publish a jittery rate.
3. **Phase steps are safe but audible.** A step produces one timeline discontinuity that clients handle, but the audio jumps. The servo should slew small phase errors and reserve steps (with a seed change) for re-acquisition after a large error.
4. **Update rate sets tracking lag.** Re-anchoring every 100 ms gave a 2 ppm lag on a fast 40 ppm/s ramp. Real oscillators drift far more slowly (well under 1 ppm/s), so 100 ms updates are ample.
