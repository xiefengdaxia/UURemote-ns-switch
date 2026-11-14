# UU Remote NS (Switch high-quality streaming fork)

**Native unofficial UU Remote client for Nintendo Switch — high-bitrate / high-quality fix branch.**

A homebrew client written in C++17 / libnx. It connects directly to the official NetEase UU
service and to the original UU Remote PC client, receives the desktop picture and audio, and
sends keyboard / mouse input from Joy-Con or the touch screen. No extra gateway is required.

> ⚠️ **Unofficial project.** Not affiliated with NetEase UU or Nintendo. This repository contains
> **no** official binaries, signing keys or assets. The signing constant required at runtime must be
> extracted by each user from their own legally obtained APK; no prebuilt NRO is shipped with a key
> embedded. See [NOTICE.md](NOTICE.md).

![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-428580)
![Status](https://img.shields.io/badge/status-0.1.64-d99a36)
[![License](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)

[Features](#features) · [What was fixed](#what-was-fixed) · [Controls](#controls) · [Building](#building) · [Limitations](#limitations) · [中文](README.md)

## What was fixed

Upstream suffers from **periodic corruption (vertical lines / color blocks), blurry picture and
frame rate dropping from 60 to 33 fps**. This branch found and fixed the root cause. Measured on
Switch hardware: **4 / 8 / 20 Mbps at 60 fps are clean, with zero packet loss**.

### Root cause

**libnx defaults to a UDP receive buffer of only `0xA500` (42,240 bytes).** Video arrives in
frame-sized bursts, so any scheduling hiccup on the network thread overflows the buffer and drops
packets (~4–8% measured). That triggers a chain reaction:

1. The receiver honestly reports loss in TWCC → the server's GCC believes the network is congested
   and drops the bitrate from 4–8 Mbps to 0.9 Mbps, fps 60 → 33.
2. The client requested a full IDR on every gap (93 in 5 minutes) → key frames consumed 10–25% of
   the bitrate → periodic quality dips.
3. The decoder reference chain broke → vertical lines / partial blur until the next key frame.

### Fixes

| Fix | Detail |
| --- | --- |
| **UDP receive buffer 42 KB → 256 KB** | Custom `udp_rx_buf_size` passed to `socketInitialize()`, falling back to defaults on failure. **This is what removes the loss.** |
| TWCC feedback encoding | `reference time` now uses the specified 64 ms unit (it was 6.25 ms — a 10× error that collapsed the bitrate to a few hundred kbps); gaps are encoded with their real receive status; intra-batch arrival times are anchored to a smoothed packet interval. |
| Keyframe requests | libdatachannel's `RtcpPli` put the remote media SSRC into the packet *sender* SSRC field and was ignored. A custom handler sends PLI + FIR with the local SSRC; measured gap→IDR time is **1 ms median**. |
| No loss-driven downshift | Radio-interference loss is no longer reported to the server's rate controller (RTCP RR zeroed as well), avoiding the "loss → lower rate → more loss" loop. |
| NAL scanning | A frame is SPS + PPS + IDR NALs concatenated; the old code only looked at the first NAL, breaking key-frame detection and the freeze logic. |
| No decoder flush | On a gap the decoder keeps its reference state (key-frame interval is long; flushing produced garbage), and the client freezes on the last good frame while requesting an IDR. |
| Frame pacing | Decoded frames go through a 2-frame buffer released at display refresh rate; shared frame pointers remove a 3.7 MB copy per frame. |
| Async diagnostics | Logging was done synchronously (`fopen/fflush/fclose` on SD, tens of ms) on the RTP receive thread, blocking packet intake. Now a background writer batches lines; the 128 KB log rollover deadlock is fixed too. |
| Local cursor | Draws a standard arrow locally when the desktop capture does not include the system cursor. |

### Measurements (Switch handheld, 5 GHz Wi-Fi)

| Item | Result |
| --- | --- |
| Link throughput (4 MB TCP) | **42 Mbps** |
| 4 / 8 / 20 Mbps @ 60 fps | Clean, no lines, `lost=0` |
| Gap → key frame | 1 ms median, 27 ms P90 |
| Decoder queue overflow | 0 |

## Features

- Large QR-code or SMS login, device list, session management.
- Native WebRTC H.264 + Opus; NVDEC hardware decoding with software fallback.
- Quality presets (L) and 30/60 fps switch (R); custom bitrate 1–20 Mbps with ZR/ZL.
- Joy-Con mouse mapping, touch pad, keyboard shortcuts.
- Diagnostics written to `connection.log` (fixed event names and numeric values only; no
  credentials, addresses, SDP or data-channel contents).

## Controls

| Context | Button | Action |
| --- | --- | --- |
| Home | ↑ / ↓ · A | Select PC · Confirm / QR login |
| Login | X | Switch QR / SMS login |
| Home (signed in) | Y · X · + | Refresh · Sign out · Exit |
| Streaming | Left stick | Move mouse |
| Streaming | ZR / ZL | Left / right click |
| Streaming | Right stick ↕ | Scroll |
| Streaming | A / B / X / Y | Enter / Esc / Space / Tab |
| Streaming | D-pad | W / S / A / D |
| Streaming | L / R | Quality / frame rate |
| Streaming | + · − | Help overlay · Disconnect |

## Building

Requires **CMake 3.21+, Ninja, Python 3.12+** and the devkitPro Switch toolchain, plus your own
**UU TV 3.5.0 APK** to extract the signing constant. The repository contains no APK or key.

```sh
git clone https://github.com/xiefengdaxia/UURemote-ns-switch.git
cd UURemote-ns-switch
python3 scripts/bootstrap_sdk.py          # Windows: fetch the pinned toolchain into .tools/
./scripts/build.ps1 -Apk 'C:\path\to\UURemote-TV-3.5.0.apk'
```

Output: `dist/UURemote-ns-<version>-switch.zip`. Copy `UURemote-ns.nro` to
`/switch/UURemote-ns/` and, if the build did not embed the key, extract it with
`python3 scripts/extract_key.py <apk> --output signing.key` and place it at
`/switch/UURemote-ns/signing.key`.

Native tests (no Switch or APK needed):

```sh
python3 scripts/fetch_deps.py
cmake -S . -B build-protocol -G Ninja -DUU_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-protocol && ctest --test-dir build-protocol --output-on-failure
```

## Limitations

- **H.265 is not enabled by the server**: the SDP answer accepts it, but packets still arrive as
  H.264 (PT 96). The client already switches decoders based on the actual RTP payload type.
- Audio may occasionally disconnect; reconnecting restores it (log marker `aud-clear`).
- No app list, clipboard or file transfer; no TURN over TCP/TLS; no automatic reconnect.
- No zero-copy display: the Switch SDL port does not support NV12 textures, so hardware frames are
  converted to RGBA on the CPU.

## License

[GPL-3.0-or-later](LICENSE). Third-party notices in [THIRD_PARTY.md](THIRD_PARTY.md);
unofficial/trademark notice in [NOTICE.md](NOTICE.md).

Upstream project: <https://github.com/Antman2023/UURemote-ns>
