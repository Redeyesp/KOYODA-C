#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KOYODA Xiaozhi Backend — Foundation
 * ====================================
 *
 * This module replaces the old ad-hoc raw-PCM-over-TCP link
 * (koyoda_audio_stream.c, still present but no longer wired into
 * app_main) with a client that speaks the protocol used by the
 * xiaozhi-esp32 project:
 *
 *   1. OTA check-in: an HTTPS POST to a configured URL, the same way
 *      xiaozhi-esp32's Ota::CheckVersion() does. The response can
 *      contain a `websocket` section (url + token) telling KOYODA
 *      which server to talk to, and a `firmware` section KOYODA can
 *      use later for real OTA updates.
 *
 *   2. WebSocket audio channel: JSON "hello"/"listen"/"tts" control
 *      messages plus binary audio frames, matching xiaozhi-esp32's
 *      WebsocketProtocol (protocol version 1: raw payload, no binary
 *      header).
 *
 * IMPORTANT — CODEC GAP (read this before wiring a real server):
 *   Stock xiaozhi-server / xiaozhi-esp32-server deployments expect
 *   Opus-encoded audio. KOYODA does not yet have an Opus codec in
 *   this tree, so this module currently sends/receives *raw 16-bit
 *   PCM* and advertises "format":"pcm" in its hello message instead
 *   of "opus". That will only work against a server you've modified
 *   to accept PCM, or against a test harness of your own. The two
 *   spots to add real Opus support are clearly marked
 *   KOYODA_TODO_OPUS_ENCODE / KOYODA_TODO_OPUS_DECODE in
 *   koyoda_backend.c. See README-XIAOZHI-BACKEND.md.
 *
 * KOYODA remains a fully working pet companion with this module
 * doing nothing at all: it only activates once Wi-Fi is connected
 * AND the user has long-pressed the face to enable AI mode
 * (koyoda_audio_duplex_ai_is_enabled()). With AI off, or with no
 * Wi-Fi, none of this code opens a socket.
 */

/* Called once from app_main(), after koyoda_wifi_start() and
 * koyoda_audio_duplex_start(). Spawns a single low-priority worker
 * task and returns immediately. */
esp_err_t koyoda_backend_start(void);

/* Called by koyoda_wifi.c whenever the station link comes up or goes
 * down. Never blocks and never touches LVGL. */
void koyoda_backend_notify_wifi_state(bool connected);

/* True once the WebSocket audio channel is open and the server's
 * "hello" reply has been received. */
bool koyoda_backend_is_channel_open(void);

/* Force a fresh OTA check-in + reconnect on the next worker tick
 * (e.g. from a future settings page). Safe to call from any task. */
void koyoda_backend_request_reconnect(void);

#ifdef __cplusplus
}
#endif
