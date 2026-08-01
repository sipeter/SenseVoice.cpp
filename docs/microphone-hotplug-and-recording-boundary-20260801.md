# Microphone hot-plug and recording boundary (2026-08-01)

## Why the five-second buffer remains

On 2026-02-28, commit `ea58d24` changed the local capture ring buffer from
`chunk_size << 2` (50 ms chunks, about 200 ms) to 5000 ms. The change protects
long recordings from losing audio while inference is busy. It is a runtime
inference buffer, not an intentional pre-roll feature.

## Recording boundary

Traditional local-microphone `START` now sets a one-shot clear request. The
audio worker clears the SDL ring buffer immediately before consuming the new
session. This keeps the five-second capacity while preventing audio spoken
before the hotkey from entering the new recording. The dual-input path already
clears its local microphone buffer at `START PC`, so its behavior is unchanged.

## Hot-plug behavior

The capture monitor checks SDL removal events, stopped devices, device
enumeration, and a stalled capture callback. It emits `[[MIC_OFFLINE]]` once
per offline episode and keeps retrying the configured capture device in the
background. The PC client presents the recovery toast; the user can reconnect
the device and use Settings -> Hardware & Devices -> Refresh microphone and
restart engine. That action is the supported deterministic recovery path.

Physical USB unplug/replug must still be verified on the target machine because
SDL/Windows backend behavior varies by device and driver.
