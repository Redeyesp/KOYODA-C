#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KOYODA Opus codec bridge (Phase 2)
 * ==================================
 *
 * Closes the "codec gap" documented in README-XIAOZHI-BACKEND.md.
 *
 * KOYODA's shared audio path runs at 22050 Hz mono 16-bit. The xiaozhi
 * protocol expects Opus at 16000 Hz mono, 60 ms frames. This module sits
 * between the two:
 *
 *   mic:  22050 PCM --resample--> 16000 PCM --accumulate 960--> Opus encode
 *   spk:  Opus decode --> 16000 PCM --resample--> 22050 PCM --> playback
 *
 * Threading contract:
 *   NONE of these functions may be called from the audio owner task.
 *   Encoding/decoding/resampling are CPU-heavy and must stay on the
 *   backend worker task so the audio path remains deterministic. The
 *   backend already routes mic audio through a queue for this reason.
 *
 * Memory contract:
 *   The Opus encoder, decoder and both resamplers together cost tens of
 *   kilobytes. koyoda_codec_open() is therefore called only when the
 *   audio channel actually opens, and koyoda_codec_close() releases
 *   everything as soon as it closes. An idle KOYODA (AI off) holds no
 *   codec memory at all -- important on this board, where internal RAM
 *   is shared with the LCD's DMA buffers.
 */

/* 60 ms @ 16 kHz == 960 samples, matching xiaozhi's OPUS_FRAME_DURATION_MS. */
#define KOYODA_OPUS_SAMPLE_RATE    16000
#define KOYODA_OPUS_FRAME_MS       60
#define KOYODA_OPUS_FRAME_SAMPLES  ((KOYODA_OPUS_SAMPLE_RATE / 1000) * KOYODA_OPUS_FRAME_MS)

/* KOYODA's native rate, both mic and speaker. */
#define KOYODA_PCM_SAMPLE_RATE     22050

/* Largest PCM block koyoda_codec_decode() can return (60 ms @22050 plus
 * headroom for resampler rounding). */
#define KOYODA_CODEC_MAX_PCM_OUT   2048

/* Called once per encoded Opus packet produced by koyoda_codec_encode_push(). */
typedef void (*koyoda_codec_packet_cb_t)(
    const uint8_t *data,
    size_t len,
    void *user_ctx);

/* Allocate encoder, decoder and resamplers. Safe to call when already
 * open (no-op). Returns ESP_OK only if the full chain came up. */
esp_err_t koyoda_codec_open(void);

/* Release everything. Safe to call when already closed. */
void koyoda_codec_close(void);

bool koyoda_codec_is_open(void);

/*
 * Feed captured mic PCM at 22050 Hz. Samples are resampled and buffered
 * internally; `cb` fires zero or more times, once per complete 60 ms
 * Opus frame. Partial frames are retained for the next call.
 */
esp_err_t koyoda_codec_encode_push(
    const int16_t *pcm_22k,
    size_t sample_count,
    koyoda_codec_packet_cb_t cb,
    void *user_ctx);

/* Drop any partially accumulated frame (e.g. at end of an utterance). */
void koyoda_codec_encode_reset(void);

/*
 * Decode one Opus packet to 22050 Hz PCM ready for
 * koyoda_audio_duplex_playback_write(). `out_samples` receives the
 * number of int16 samples written, never more than out_capacity.
 */
esp_err_t koyoda_codec_decode(
    const uint8_t *opus_data,
    size_t opus_len,
    int16_t *out_pcm_22k,
    size_t out_capacity,
    size_t *out_samples);

#ifdef __cplusplus
}
#endif
