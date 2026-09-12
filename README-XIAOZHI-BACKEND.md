# KOYODA × Xiaozhi Backend — Foundation

This is your KOYODA firmware with a new networking/backend layer grafted
on, built to speak the same OTA check-in + WebSocket protocol as
[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32), instead of the old
ad-hoc raw-PCM-over-TCP link to `tools/koyoda_pcm_receiver.py`.

**Nothing about the pet was touched.** Face, blink, sleep animations,
battery page, Wi-Fi status page, volume page, swipe navigation, charging
animation, the AI long-press toggle, the mic/VAD pipeline — all identical
to your original tree. KOYODA boots, looks, and behaves exactly the same
with Wi-Fi off, with AI off, or with no server reachable at all. The new
code only ever runs when Wi-Fi is connected **and** you've long-pressed
the face to turn AI on.

## What was actually done, file by file

| File | Change |
|---|---|
| `main/koyoda_backend.h` / `.c` | **New.** OTA check-in, WebSocket client, JSON protocol, mic→server and server→speaker bridging. |
| `main/koyoda_wifi.c` | Two one-line hooks added: on `IP_EVENT_STA_GOT_IP` and `WIFI_EVENT_STA_DISCONNECTED`, it calls `koyoda_backend_notify_wifi_state(true/false)`. Nothing else changed. |
| `main/main.c` | The old `koyoda_audio_stream_start()` call (raw TCP link) is replaced with `koyoda_backend_start()`. Everything else in `app_main()` and the UI loop is untouched. |
| `main/koyoda_audio_stream.c/.h` | **Kept in the repo, no longer compiled.** Left as reference / a fallback you can wire back in if you ever want the old PC-tool link again. |
| `main/CMakeLists.txt` | Swapped the stream source for `koyoda_backend.c`; added `esp_http_client`, `esp_https_ota`, `app_update`, `esp_websocket_client`, `mbedtls`, `json` to `REQUIRES` (all built into ESP-IDF, no component-manager download needed). |
| `main/Kconfig.projbuild` | Added a **"KOYODA Xiaozhi Backend"** menu (OTA URL, board name). The old stream host/port menu is kept but marked legacy/unused. |
| `sdkconfig.defaults` | Enabled the mbedtls certificate bundle so HTTPS/WSS TLS verification works out of the box. |

Nothing in `koyoda_idle.c`, `koyoda_sleep_*.c`, `koyoda_blink_patches.c`,
`koyoda_ai_overlays.c`, `koyoda_charge_composite.c`,
`koyoda_listening_notice.c`, `koyoda_face_state.c`, `pmu_bridge.cpp`, the
`assets/`, `partitions.csv`, or `components/XPowersLib` was touched.

## How the new backend behaves

```
Boot
 └─ AI mode OFF (as before). Pet works fully standalone.
 └─ Long-press the face → AI mode ON
     └─ If Wi-Fi is up: koyoda_backend does, in order:
         1. POST device info to CONFIG_KOYODA_OTA_URL (HTTPS)
            → server replies with { "websocket": { "url", "token" } }
         2. Opens a WebSocket to that url, sends a "hello" JSON message
         3. Waits for the server's "hello" reply (10 s timeout)
         4. Channel open → face goes to IDLE (the existing "listening"
            double-blink indicator lights up, unchanged from before)
     └─ On VAD speech-start/stop, sends {"type":"listen","state":...}
        and streams mic audio as binary WebSocket frames
     └─ On {"type":"tts","state":"start"} → face SPEAKING, audio played
        back through koyoda_audio_duplex_playback_*
     └─ On {"type":"tts","state":"stop"} → face back to IDLE
 └─ Long-press again → AI OFF → channel closes immediately, pet is just
    a pet again.
```

Wi-Fi loss or a WebSocket error at any point closes the channel and
returns the face to IDLE; it retries automatically (30 s backoff) as
long as AI mode stays on and Wi-Fi comes back.

## Audio codec (Phase 2 — implemented)

KOYODA now speaks real Opus, matching xiaozhi-esp32's parameters:
**16000 Hz, mono, 60 ms frames** (`OPUS_FRAME_DURATION_MS 60`).

Because KOYODA's own audio path runs at 22050 Hz — which is *not* a legal
Opus rate — `main/koyoda_codec.c` resamples in both directions:

```
mic:  22050 PCM --resample--> 16000 --accumulate 960--> Opus encode --> WS
spk:  WS --> Opus decode --> 16000 --resample--> 22050 --> playback
```

Components used are the same ones xiaozhi-esp32 uses:
`espressif/esp_audio_codec` (Opus) and `espressif/esp_audio_effects`
(rate conversion).

Two deliberate design points:

- **All codec work runs on the backend worker task**, never the audio
  owner task. The audio callback only posts to a queue and returns, so
  the deterministic audio path is preserved.
- **The codec is opened only when the audio channel opens** and released
  the moment it closes. Opus state costs tens of KB; an idle KOYODA
  (AI off) holds none of it. Buffers prefer PSRAM, because internal RAM
  on this board is shared with the LCD's DMA pool.

## OTA / activation parity (implemented)

The check-in now matches xiaozhi-esp32's `Ota::CheckVersion()`:

**Headers sent:** `Activation-Version`, `Device-Id`, `Client-Id`,
`User-Agent`, `Accept-Language`, `Content-Type`.
(`Serial-Number` is not sent — it requires an eFuse-burned per-device
secret that KOYODA has no provisioning flow for, so KOYODA advertises
`Activation-Version: 1`.)

**Body** follows `Board::GetSystemInfoJson()`: version 2, language,
flash/PSRAM size, minimum free heap, MAC, UUID, chip info, full
application block (name, version, compile time, IDF version, ELF
SHA-256) and a board block with the current SSID/RSSI. The large
`partition_table` array xiaozhi also sends is omitted deliberately —
servers key off `mac_address`/`uuid`, and building it would cost several
KB of heap on every reconnect.

**Response sections parsed:** all five — `activation`, `mqtt`,
`websocket`, `server_time`, `firmware`.

- If the server returns an **activation code**, KOYODA prints it as a
  banner in the serial log and keeps retrying. Enter the code in the
  xiaozhi console and it connects on the next attempt.
- `websocket.version` is now read and persisted to NVS instead of being
  hard-coded, and is used for both the `Protocol-Version` header and the
  `hello` message.
- `server_time` sets the system clock, which also makes TLS certificate
  validity checks behave.
- `firmware` is reported in the log but **never auto-flashed**. This is
  deliberate: a stray or hostile check-in response must not be able to
  silently reflash a running pet.

### Still not at full xiaozhi parity

- **Binary framing for protocol v2/v3 is not implemented.** KOYODA now
  *negotiates* the version but only speaks v1 (bare payload, no header).
  v2 prepends a 16-byte header and v3 a 4-byte one. If a server hands
  back version 2 or 3, audio framing will be wrong. This is the next
  phase.
- **Challenge-response activation (Activation-Version 2)** is not
  supported; KOYODA logs a warning if a server sends a challenge.
- **No wake word / AFE.** KOYODA uses its own VAD; there is no `esp-sr`
  integration.

## Setup checklist

1. Open `idf.py menuconfig` → **KOYODA Xiaozhi Backend**:
   - Set **OTA check-in URL** to your own server (self-hosted
     xiaozhi-esp32-server or a test harness) — do *not* point a hobby
     board at a production public endpoint you don't control.
   - Optionally set **Board identifier**.
2. Build/flash as usual: `idf.py build flash monitor`.
3. Provision Wi-Fi exactly like before (`KOYODA-Setup` AP,
   `koyoda88`).
4. Long-press the face for ~1.2 s to turn AI on. Watch the serial log
   for `KOYODA_BACKEND` lines — it logs every state transition (OTA
   check-in, WebSocket connect, hello, channel open/close).
5. Confirm your server actually returns a `websocket` section from the
   OTA URL — without it, KOYODA will keep retrying every 30 s and
   logging a warning, and the pet will just sit there fully functional
   with AI "on" but no active connection (by design — no silent
   failures that touch the UI).

## Honesty note

This was written and reviewed by hand against ESP-IDF's
`esp_websocket_client`/`esp_http_client` APIs and xiaozhi-esp32's actual
`protocols/websocket_protocol.cc` / `ota.cc` source, but it has **not**
been compiled or flashed — there's no ESP-IDF toolchain or network
access in the environment this was built in. Treat it as a solid,
carefully-reasoned starting point, not a "just works" drop-in. Before
you trust it on real hardware:

- Run a clean `idf.py build` and fix any header/API-name drift for the
  exact ESP-IDF version you're on (this was written against the
  IDF 5.x `esp_websocket_client` / `esp_http_client` API surface).
- The WebSocket data handler assumes each `WEBSOCKET_EVENT_DATA` is one
  complete frame; large frames can arrive fragmented
  (`payload_offset`/`payload_len`) and aren't reassembled yet — fine
  for small JSON control messages, worth hardening before relying on
  large audio frames.
- The mic → server path currently forwards **raw PCM only while VAD
  says someone is speaking** (matching your existing duplex module's
  half-duplex design); tune `BACKEND_MIC_SAMPLES_MAX` /
  `BACKEND_MIC_QUEUE_DEPTH` in `koyoda_backend.c` if you see dropped
  frames in the log once real audio is flowing.
