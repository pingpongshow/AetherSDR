# DAX on Windows (device-mapped DAX)

AetherSDR on macOS ships its own CoreAudio driver for DAX ("AetherSDR DAX 1–4",
"AetherSDR TX"), and on Linux it creates PulseAudio/PipeWire pipe devices.
Windows has neither: a virtual audio device on Windows means a kernel-mode
(WDM/AVStream) driver, and Windows will only load one that Microsoft has signed.

So on Windows AetherSDR sends DAX audio to **ordinary audio devices you choose**,
usually a third-party virtual audio cable. This is the **device-mapped** DAX
backend (`DaxBackend=DeviceMapped`). Upstream tracking: issue #87 (the DAX IQ
comment there recommends VB-Audio Virtual Cable as the first step).

```
radio ──VITA-49──▶ AetherSDR ──DAX RX 1──▶ "CABLE Input"  ═══▶ "CABLE Output"  ──▶ WSJT-X input
                               DAX TX  ◀── "CABLE-A Output" ◀═══ "CABLE-A Input" ◀── WSJT-X output
```

A virtual cable is a pair of endpoints. What you play into its *Input*
(a playback device) comes out of its *Output* (a recording device).

## Recommended setup: VB-Audio Virtual Cable

1. Install **VB-CABLE** (free) from vb-audio.com. It adds one cable:
   `CABLE Input (VB-Audio Virtual Cable)` / `CABLE Output (VB-Audio Virtual Cable)`.
2. If you want DAX **TX** as well, you need a second, separate cable. Install
   **VB-CABLE A+B** (donationware) for `CABLE-A …` and `CABLE-B …`.
   VAC (Virtual Audio Cable, paid) works the same way with `Line 1`, `Line 2`, ….
3. Reboot if the installer asks you to.
4. In AetherSDR open **Settings → DAX Audio Devices…** and click **Auto-detect**.
   With VB-CABLE + A+B the suggested mapping is:

   | AetherSDR          | device AetherSDR uses                  | select in WSJT-X / VARA / fldigi |
   |--------------------|----------------------------------------|---------------------------------|
   | DAX RX 1 →         | `CABLE Input (VB-Audio Virtual Cable)` | Input: `CABLE Output (VB-Audio Virtual Cable)` |
   | DAX TX ←           | `CABLE-A Output (VB-Audio Cable A)`    | Output: `CABLE-A Input (VB-Audio Cable A)` |
   | DAX RX 2 →         | `CABLE-B Input (VB-Audio Cable B)`     | Input: `CABLE-B Output (VB-Audio Cable B)` |

   The right-hand column shows what to select in the digital-mode program.
5. Click **OK**, then enable DAX in the DAX applet as usual.

**Never use the same cable for DAX RX and DAX TX.** Received audio would go
straight back to the transmitter. The dialog will not save such a mapping,
and the backend refuses to open it.

### Windows sound settings

* Set each cable's format (Sound Control Panel → device → Properties →
  Advanced) to **48000 Hz**, 16- or 24-bit, and set the *Input* and *Output*
  side of a cable to the **same** rate. AetherSDR converts to whatever the
  device runs at, but a rate mismatch inside the cable driver makes the cable
  itself resample.
* Keep the virtual cables away from being the Windows default playback or
  recording device. Otherwise system sounds and Teams calls end up on the air.
* Turn off "Allow applications to take exclusive control" if another program
  tries to grab the cable in exclusive mode.

### Voicemeeter

Voicemeeter's VAIO/AUX buses are detected, but they are ranked last because
they are **mixer buses**. Make sure the bus you give DAX is not also routed to
your speakers, or to a hardware output that feeds the radio.

## How the backend works

`src/core/DeviceMappedDax.{h,cpp}`, plus its device-independent parts in
`src/core/DaxDevicePipeline.{h,cpp}` (unit tests:
`tests/device_mapped_dax_test.cpp`). `VirtualAudioBridge` picks the backend.

* **Data formats are the same as the native bridges.** DAX RX comes in from
  `PanadapterStream::daxAudioReady` as float32 stereo at 24 kHz. DAX TX leaves
  as float32 stereo at 24 kHz through `txAudioReady` →
  `AudioEngine::feedDaxTxAudio`.
* **RX path** (per channel): channel gain → channel remap (stereo kept on
  2-channel devices, (L+R)/2 on mono ones, like the Linux bridge) → windowed-sinc
  resampler to the device rate (usually 48 kHz) → lock-free SPSC playout
  buffer → `QAudioSink` in pull mode. The pull callback never blocks and never
  returns short: when there is not enough audio it pads with silence, so the
  device clock keeps running through TX. This is the same reason the native
  bridges feed silence during TX, and it keeps WSJT-X DT from drifting.
* **Clock drift:** the radio's 24 kHz clock and the sound card clock drift
  apart by tens of ppm. The playout buffer tracks a smoothed (0.5 s) fill
  level against its target (40 ms plus one device period). Outside ±5 ms it
  drops or repeats one frame, at most once per 50 ms (≤ 0.04 %, inaudible
  to modems). A backlog over ~170 ms (for example after a GUI stall) resyncs
  straight back to the target.
* **TX path:** `QAudioSource` in push mode → device format to float → stereo
  (a mono device is duplicated) → resample to 24 kHz → TX gain → emitted in
  128-frame chunks. When more than 32 ms is queued it skips to the newest
  ~10.7 ms, the same low-latency rule as the macOS shared-memory TX poller.
* **Device formats:** each device is tried at its own preferred rate first,
  then 48 / 44.1 / 24 kHz, with float before int16/int32 and stereo before mono.
  On Windows every candidate is tried with `start()`, because
  `isFormatSupported()` is unreliable under WASAPI shared mode (see
  AudioEngine). WASAPI device buffers are 30 ms (three engine periods). Other
  platforms use 20 ms.
* **Hot-plug / session loss** (Windows and macOS): if a mapped device appears,
  disappears or stops with an error, only that channel is reopened.

Expected latency is about 40 ms plus one device period plus the cable's own
buffer. On macOS with Qt 6.9, pull mode reads 2048 frames (≈ 43 ms) at a time,
so that path measured about 70–85 ms. That is fine for FT8/FT4 (DT tolerance
is ±0.5 s) and for VARA.

### Settings (AppSettings)

| key | values | default |
|-----|--------|---------|
| `DaxBackend` | `Native` \| `DeviceMapped` | `DeviceMapped` on Windows, `Native` on macOS/Linux |
| `DaxRxDevice1` … `DaxRxDevice4` | output device description, empty = channel off | empty |
| `DaxTxDevice` | input device description, empty = DAX TX off | empty |

`DaxRxGain1..4` and `DaxTxGain` are unchanged and apply to both backends.

### Using it on macOS / Linux

Device-mapped DAX also works on macOS (for example with BlackHole or Rogue
Amoeba Loopback, when the bundled HAL driver can't be installed) and on Linux
(snd-aloop, or a PipeWire null sink + monitor). To use it, pick
"Map to audio devices" in the same dialog. It is opt-in there; the default
stays Native.

## Future: a native Windows DAX driver

SmartSDR for Windows installs its own "DAX Audio RX n / TX" devices. Doing the
same for AetherSDR would take:

1. **Driver:** a WaveRT miniport based on Microsoft's **SysVAD** sample
   (Windows-driver-samples/audio/sysvad), trimmed to one render and one
   capture endpoint per DAX channel ("AetherSDR DAX 1–4", "AetherSDR TX"),
   48 kHz and 24 kHz, 16/24/32-bit. The data path to user mode would be a
   shared ring buffer much like `DaxShmBlock`: a section object the driver
   maps, or an IOCTL-registered user buffer, polled by AetherSDR. Glitch-free
   operation needs care with the WaveRT position register and with
   notifications/timers.
   Microsoft's smaller `simpleaudiosample` (in the same repository) is also
   worth evaluating as a starting point. Either way it is a kernel-mode driver:
   Windows has no user-mode API for creating audio endpoints.
2. **Build:** WDK + Visual Studio in CI (a separate `driver/` project, not
   part of the CMake app build), with x64 and ARM64 builds.
3. **Signing:**
   * An **EV code-signing certificate** registered to the project's legal
     entity, on an HSM token or a cloud HSM. It is needed to register with the
     Windows Hardware Dev Center.
   * **Attestation signing** through Partner Center: submit a CAB signed with
     the EV cert, and Microsoft returns a driver signed by Microsoft that
     Windows 10/11 client SKUs load. This means no HLK run, but also no
     Windows Server support and no "Windows Hardware Compatible" logo.
   * WHQL/HLK certification only if Server support or the logo is wanted.
   * Secure Boot and HVCI/Memory Integrity require the driver to be
     HVCI-compatible (the `/INTEGRITYCHECK` flag, no executable pool).
4. **Install:** the Inno Setup installer would ship the signed
   package (`.inf`/`.sys`/`.cat`) and install it with `pnputil /add-driver
   … /install` as a root-enumerated device. Uninstall must remove it cleanly.
   Driver updates need a version bump in the INF.
5. **Cost and risk:** an annual EV certificate (~US$300–600/yr), plus a
   kernel-mode crash surface (a DAX bug becomes a BSOD) and ongoing
   maintenance for each Windows release.

Until that exists, device-mapped DAX with VB-Cable is the supported path.
Because it goes through the regular Qt Multimedia / WASAPI APIs, it would stay
as the fallback even after a native driver ships.
