# Spike S1: Can PTP run inside the Core Audio driver host?

Date: 2 October 2026. macOS 26.0.1, Apple Silicon (arm64).
Plan: [Step2-Clocking-Plan.md](../Step2-Clocking-Plan.md).

## Question

The driver runs inside Apple's sandboxed Core Audio driver host, not as root. Can it bind the PTP ports (UDP 319/320), join the PTP multicast group, get kernel receive timestamps in host time, and send? If not, PTP has to move to a separate launchd daemon.

## Method

`Tools/SandboxProbe/` builds a throwaway HAL plug-in that publishes no device (CMake option `BUILD_SPIKES=ON`, target `AES67SandboxProbe`). When Core Audio loads it, it runs each check on a background thread and logs PASS/FAIL to the unified log under subsystem `com.aes67driver.probe`.

While it listened for 30 seconds, a user-space script sent marker packets to 224.0.1.129 on both ports. These were TTL 0, so they stayed on this Mac, and versionPTP 0, so they are not valid PTP. The probe sent one packet of its own the same way.

The probe ran twice: once with `AudioServerPlugIn_Network` in its Info.plist and once with the key removed.

To reproduce:
1. Build `AES67SandboxProbe`.
2. Copy the bundle to `/Library/Audio/Plug-Ins/HAL/` and restart coreaudiod.
3. Read the results with: `sudo log show --last 5m --info --debug --predicate 'subsystem == "com.aes67driver.probe"'`. Plain `log show` omits these entries.

## Results

| Check | With network key | Without network key |
|---|---|---|
| Host process | `Core Audio Driver (AES67SandboxProbe.driver)`, uid 202 (`_coreaudiod`) | same |
| Bind ephemeral port (control) | PASS | PASS |
| Bind UDP 319 / 320 | PASS / PASS | PASS / PASS |
| `SO_TIMESTAMP_MONOTONIC` | PASS | PASS |
| Join 224.0.1.129 (both sockets) | PASS | PASS |
| Send to 224.0.1.129:319 | PASS | PASS |
| Packets received (319 / 320) | 149 / 147, all with kernel monotonic timestamps | 148 / 146, all with timestamps |
| Kernel-to-userspace delay | 6.5 us to 31.7 ms | 4.8 us to 9.4 ms |

A user-space test outside the sandbox confirmed separately that `SCM_TIMESTAMP_MONOTONIC` values are `mach_absolute_time` ticks, the time base Core Audio uses for zero timestamps.

## Conclusions

1. **PTP can run in-process.** No launchd daemon is needed, which resolves decision 3 in the plan.
2. **Kernel timestamps are mandatory, not an optimisation.** Threads in the driver host were scheduled up to 31.7 ms after a packet arrived. A PTP slave that read the clock in user space would see that as path-delay noise. With `SO_TIMESTAMP_MONOTONIC` the arrival time is captured in the kernel, already in host-time units.
3. **The sandbox did not enforce `AudioServerPlugIn_Network` on this macOS version.** Keep the key anyway: `AudioServerPlugIn.h` documents it as required, and enforcement could change.
4. **Nothing else on this Mac owns ports 319/320.** With another PTP stack present (for example another AoIP driver), `SO_REUSEADDR`/`SO_REUSEPORT` let both bind; this was not tested here.

## Not covered (carried to spike S3)

- **No real PTP traffic.** There was no grandmaster on the network during the test: the probe received only the markers. Receiving PTP from a real device on the LAN, and the timestamp accuracy that results, is what S3 measures with real hardware.
- **No transmit timestamps.** The probe did not exercise them; macOS offers no kernel send timestamp for UDP, so Delay_Req send times (t3) will be taken in user space just after `sendto`, as the plan assumes.
