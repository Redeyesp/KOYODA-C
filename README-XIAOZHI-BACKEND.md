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

## ⚠️ The one real gap: audio codec

Stock xiaozhi-server / xiaozhi-esp32-server deployments expect
**Opus-encoded audio at 16 kHz**. This foundation does **not** include an
Opus codec yet — it sends/receives raw 16-bit PCM at KOYODA's native
22050 Hz and honestly advertises `"format":"pcm"` in its hello message
instead of lying about Opus.

That means: **out of the box, this will complete the OTA check-in and
open the WebSocket, but an unmodified xiaozhi-server won't understand
the audio you send it**, and audio it sends back (Opus) will just sound
like noise if played as raw PCM.

Two ways forward, both left as clearly marked TODOs in
`koyoda_backend.c`:

1. **Add Opus** — pull in the `esp-opus` / `esp_audio_codec` managed
   component (the same one xiaozhi-esp32 uses), and encode/decode at
   the two marked spots: `KOYODA_TODO_OPUS_ENCODE` (mic → server) and
   `KOYODA_TODO_OPUS_DECODE` (server → speaker). This is the "do it
   properly" path and is what you'd want for any real deployment.
2. **Patch your own server** — if you're running your own
   xiaozhi-esp32-server fork for testing, add a PCM passthrough mode
   that skips Opus decode/encode when it sees `"format":"pcm"`. Much
   faster to get a first end-to-end voice round-trip working while you
   build out the rest.

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
