# New Modes

This file tracks the requested additional operating modes and the intended user-facing grouping for the `Mode` menu.

## Core Interactive Modes

| Mode Family | Direction | Status | Notes |
| --- | --- | --- | --- |
| RTTY | RX / TX | Complete | Built-in RX/TX dialog with real Baudot decode, FSK modulation, presets, slice sync, DAX TX routing, and CSV logging. |
| PSK | RX / TX | Complete | Built-in RX/TX dialog with band presets, PSK mode presets, varicode decoding, BPSK/QPSK modulation, slice sync, DAX TX routing, and CSV logging. |
| SSTV | RX / TX | Complete | Built-in RX/TX SSTV dialog with auto/manual receive mode selection, full analog mode preset list, live image decode, TX waveform generation, slice/DAX routing, image load/save, CSV logging, Bell-202 AFSK/HDLC SSDV RX/TX transport, and SSDV packet file import/export support. |
| FT8 | RX / TX | Partial | Dedicated weak-signal dialog now decodes panadapter audio natively in-process using `ft8_lib`, with slice sync, presets, replay, filtering, and map view. Native scheduled TX is integrated through the internal DAX TX audio path. |
| FT4 | RX / TX | Partial | Uses the same native in-process `ft8_lib` path as FT8, with live panadapter-audio decode, presets, replay, filtering, map view, and native scheduled TX through the internal DAX TX audio path. |
| JT65 | RX / TX | Partial | Native JT65 transmit is integrated in-process using a vendored encoder path and internal DAX TX audio scheduling. RX decoding is still pending. |
| WSPR | RX / TX | Partial | Native WSPR beacon transmit is integrated in-process using a vendored encoder path and internal DAX TX audio scheduling. RX decoding is still pending. |

## Additional Requested Modes

| Mode | Direction | Status | Notes |
| --- | --- | --- | --- |
| BPSK | RX / TX | Complete | Implemented in the PSK family dialog as BPSK-31, BPSK-63, and BPSK-125. |
| QPSK | RX / TX | Complete | Implemented in the PSK family dialog as QPSK-31, QPSK-63, and QPSK-125. |
| PSKR | RX / TX | Pending | Requested as a distinct PSK-family operating mode. |
| Domino | RX / TX | Pending | DominoEX-family digital text mode. |
| Hellschreiber | RX / TX | Pending | Feld Hell / Hellschreiber text imaging mode. |
| MFSK / MT63 | RX / TX | Pending | Multi-frequency shift keying family including MT63. |
| OLIVIA | RX / TX | Pending | Robust MFSK text mode. |
| THROB / THROBX | RX / TX | Pending | Throb-family keyboard digital modes. |
| WWV / CHU / BPM | RX | Complete | Native time-stations dialog with WWV / WWVH, CHU, and BPM profiles, profile-specific presets, slice sync, one-second pulse lock, WWV minute-frame decode, CHU Bell 103 burst decode, and frame history. |
| 2G ALE | RX | Complete | Native 8-FSK receive dialog with symbol-phase search, repeated-word majority recovery, deinterleaving, Golay correction, preamble/text extraction, and live frame history. |
| DRM | RX | Pending | Digital Radio Mondiale receive-only decode. |
| HFDL | RX | Pending | HF Data Link receive-only decoding. |
| NAVTEX / DSC | RX | Pending | Maritime text and distress receive-only decoding. |
| Timecode | RX | Complete | Implemented together with the time-stations dialog for WWV / WWVH, CHU, and BPM with UTC/status display and decoded frame history where supported. |

## UI Families

- `RTTY`: manual frequency, preset frequency list, mark/shift presets, RX/TX controls.
- `PSK`: band presets, PSK mode presets, RX/TX offset controls.
- `SSTV`: RX image view, TX image queue, mode preset list, save/load support.
- `Weak Signal`: FT8/FT4 native RX/TX with band presets, replay, station/message grid, and map view; JT65/WSPR native TX is integrated and RX decode remains pending.
- `Additional Modes`: Domino, Hellschreiber, MFSK / MT63, OLIVIA, THROB / THROBX, Time Stations, 2G ALE, DRM, HFDL, NAVTEX / DSC.

## Delivery Constraint

Only modes marked `Complete` have a working end-to-end implementation in AetherSDR. `Partial` means the mode has a real integrated path, but not the full RX/TX feature set yet. Remaining entries stay pending until they have real synchronization, framing, symbol decoding, message parsing, logging, and, for TX modes, a working modulation path or control integration.
