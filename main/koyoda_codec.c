#include "koyoda_codec.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "esp_audio_enc.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_ae_rate_cvt.h"

static const char *TAG = "KOYODA_CODEC";

/* Accumulator must hold at least one full Opus frame plus one resampled
 * input block of slack before it is drained. */
/* Accumulator capacity is computed at open() from the encoder-reported
 * frame size; see s_enc_acc_capacity. */

/*
 * Hard floor for the encoder output buffer. RFC 6716 allows a mono Opus
 * packet up to 1275 bytes; the nominal size the library reports is much
 * smaller and is NOT a guaranteed maximum. Plus slack so a library that
 * writes a few extra bytes still cannot reach neighbouring allocations.
 */
#define ENC_OUT_MAX_OPUS 1500

/* Scratch for one resampled input block. The backend feeds 256-sample
 * blocks, which shrink to ~186 at 16 kHz, but allow generous headroom. */
#define RESAMP_IN_SCRATCH 1024

static void *s_encoder = NULL;
static void *s_decoder = NULL;
static void *s_in_resampler = NULL;   /* 22050 -> 16000 */
static void *s_out_resampler = NULL;  /* 16000 -> 22050 */

static int16_t *s_enc_acc = NULL;     /* 16 kHz PCM awaiting a full frame */
static size_t s_enc_acc_len = 0;
static size_t s_enc_acc_capacity = 0;
static int16_t *s_resamp_scratch = NULL;
static uint8_t *s_enc_out = NULL;
static size_t s_enc_out_size = 0;
static size_t s_enc_frame_samples = KOYODA_OPUS_FRAME_SAMPLES;
static int16_t *s_dec_pcm16k = NULL;  /* decoder output before resampling */

static bool s_open = false;

static void free_all(void)
{
    if (s_encoder != NULL)
    {
        esp_opus_enc_close(s_encoder);
        s_encoder = NULL;
    }
    if (s_decoder != NULL)
    {
        esp_opus_dec_close(s_decoder);
        s_decoder = NULL;
    }
    if (s_in_resampler != NULL)
    {
        esp_ae_rate_cvt_close(s_in_resampler);
        s_in_resampler = NULL;
    }
    if (s_out_resampler != NULL)
    {
        esp_ae_rate_cvt_close(s_out_resampler);
        s_out_resampler = NULL;
    }

    free(s_enc_acc);
    s_enc_acc = NULL;
    free(s_resamp_scratch);
    s_resamp_scratch = NULL;
    free(s_enc_out);
    s_enc_out = NULL;
    free(s_dec_pcm16k);
    s_dec_pcm16k = NULL;

    s_enc_acc_len = 0;
    s_enc_out_size = 0;
    s_enc_acc_capacity = 0;
}

/* Prefer PSRAM for the bulk buffers: internal RAM on this board is shared
 * with the LCD's DMA pool and starving it causes visible tearing. */
static void *alloc_pref_psram(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (p == NULL)
    {
        p = malloc(bytes);
    }
    return p;
}

esp_err_t koyoda_codec_open(void)
{
    if (s_open)
    {
        return ESP_OK;
    }

    /* ---- Opus encoder: 16 kHz mono, 60 ms frames ---- */
    esp_opus_enc_config_t enc_cfg = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = ESP_OPUS_BITRATE_AUTO,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
        .complexity = 0,
        .enable_fec = false,
        .enable_dtx = true,
        .enable_vbr = true,
    };
    if (esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &s_encoder) != ESP_AUDIO_ERR_OK ||
        s_encoder == NULL)
    {
        ESP_LOGE(TAG, "Opus encoder open failed");
        free_all();
        return ESP_FAIL;
    }

    int enc_in_size = 0;
    int enc_out_size = 0;
    esp_opus_enc_get_frame_size(s_encoder, &enc_in_size, &enc_out_size);

    /*
     * Frame size comes from the encoder, not from our own arithmetic.
     * Assuming 960 samples silently breaks if the library disagrees.
     */
    if (enc_in_size > 0)
    {
        s_enc_frame_samples = (size_t)enc_in_size / sizeof(int16_t);
    }
    else
    {
        s_enc_frame_samples = KOYODA_OPUS_FRAME_SAMPLES;
    }

    /*
     * CRITICAL: do not trust the reported output size as a hard cap.
     * esp_opus_enc_get_frame_size() reports a nominal size (227 bytes at
     * our settings), but Opus VBR can emit up to ~1275 bytes for a mono
     * frame. Allocating only the nominal size lets a loud or complex
     * frame write past the buffer and corrupt the heap -- which shows up
     * later as an unrelated crash in malloc/queue code.
     */
    size_t reported = (enc_out_size > 0) ? (size_t)enc_out_size : 0;
    s_enc_out_size = (reported > ENC_OUT_MAX_OPUS) ? reported : ENC_OUT_MAX_OPUS;

    ESP_LOGI(TAG,
             "Opus encoder: in %d B (%u samples), out nominal %d B, allocating %u B",
             enc_in_size, (unsigned)s_enc_frame_samples,
             enc_out_size, (unsigned)s_enc_out_size);

    /* ---- Opus decoder: decode at 16 kHz, resample to 22050 after ----
     * 22050 is not a valid Opus rate, so unlike xiaozhi (whose codecs run
     * at 16/24/48k) KOYODA must always resample on the way out. */
    esp_opus_dec_cfg_t dec_cfg = {
        .sample_rate = KOYODA_OPUS_SAMPLE_RATE,
        .channel = ESP_AUDIO_MONO,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
        .self_delimited = false,
    };
    if (esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &s_decoder) != ESP_AUDIO_ERR_OK ||
        s_decoder == NULL)
    {
        ESP_LOGE(TAG, "Opus decoder open failed");
        free_all();
        return ESP_FAIL;
    }

    /* ---- Resamplers ---- */
    esp_ae_rate_cvt_cfg_t in_cfg = {
        .src_rate = KOYODA_PCM_SAMPLE_RATE,
        .dest_rate = KOYODA_OPUS_SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .complexity = 2,
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
    };
    if (esp_ae_rate_cvt_open(&in_cfg, &s_in_resampler) != ESP_AE_ERR_OK ||
        s_in_resampler == NULL)
    {
        ESP_LOGE(TAG, "Input resampler open failed");
        free_all();
        return ESP_FAIL;
    }

    esp_ae_rate_cvt_cfg_t out_cfg = {
        .src_rate = KOYODA_OPUS_SAMPLE_RATE,
        .dest_rate = KOYODA_PCM_SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .complexity = 2,
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
    };
    if (esp_ae_rate_cvt_open(&out_cfg, &s_out_resampler) != ESP_AE_ERR_OK ||
        s_out_resampler == NULL)
    {
        ESP_LOGE(TAG, "Output resampler open failed");
        free_all();
        return ESP_FAIL;
    }

    /* ---- Buffers ---- */
    s_enc_acc_capacity = s_enc_frame_samples * 2 + RESAMP_IN_SCRATCH;
    s_enc_acc = alloc_pref_psram(s_enc_acc_capacity * sizeof(int16_t));
    s_resamp_scratch = alloc_pref_psram(RESAMP_IN_SCRATCH * sizeof(int16_t));
    s_enc_out = alloc_pref_psram(s_enc_out_size);
    s_dec_pcm16k = alloc_pref_psram(KOYODA_OPUS_FRAME_SAMPLES * 2 * sizeof(int16_t));

    if (s_enc_acc == NULL || s_resamp_scratch == NULL ||
        s_enc_out == NULL || s_dec_pcm16k == NULL)
    {
        ESP_LOGE(TAG, "Codec buffer allocation failed");
        free_all();
        return ESP_ERR_NO_MEM;
    }

    s_enc_acc_len = 0;
    s_open = true;
    ESP_LOGI(TAG,
             "Opus ready: mic %d->%d Hz, %d ms frames (%d samples), out buf %u B",
             KOYODA_PCM_SAMPLE_RATE, KOYODA_OPUS_SAMPLE_RATE,
             KOYODA_OPUS_FRAME_MS, KOYODA_OPUS_FRAME_SAMPLES,
             (unsigned)s_enc_out_size);
    return ESP_OK;
}

void koyoda_codec_close(void)
{
    if (!s_open)
    {
        return;
    }
    s_open = false;
    free_all();
    ESP_LOGI(TAG, "Opus codec released");
}

bool koyoda_codec_is_open(void)
{
    return s_open;
}

void koyoda_codec_encode_reset(void)
{
    s_enc_acc_len = 0;
}

esp_err_t koyoda_codec_encode_push(
    const int16_t *pcm_22k,
    size_t sample_count,
    koyoda_codec_packet_cb_t cb,
    void *user_ctx)
{
    if (!s_open || pcm_22k == NULL || cb == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_count == 0)
    {
        return ESP_OK;
    }

    /* 1. Resample this block 22050 -> 16000. */
    uint32_t max_out = 0;
    if (esp_ae_rate_cvt_get_max_out_sample_num(
            s_in_resampler, sample_count, &max_out) != ESP_AE_ERR_OK)
    {
        return ESP_FAIL;
    }
    if (max_out > RESAMP_IN_SCRATCH)
    {
        ESP_LOGW(TAG, "Resampler wants %u samples, scratch holds %d; dropping block",
                 (unsigned)max_out, RESAMP_IN_SCRATCH);
        return ESP_ERR_NO_MEM;
    }

    uint32_t produced = max_out;
    if (esp_ae_rate_cvt_process(
            s_in_resampler,
            (esp_ae_sample_t)pcm_22k, (uint32_t)sample_count,
            (esp_ae_sample_t)s_resamp_scratch, &produced) != ESP_AE_ERR_OK)
    {
        return ESP_FAIL;
    }

    /* 2. Accumulate and emit whole 60 ms frames. */
    size_t consumed = 0;
    while (consumed < produced)
    {
        size_t space = s_enc_acc_capacity - s_enc_acc_len;
        size_t take = produced - consumed;
        if (take > space)
        {
            take = space;
        }
        memcpy(&s_enc_acc[s_enc_acc_len], &s_resamp_scratch[consumed],
               take * sizeof(int16_t));
        s_enc_acc_len += take;
        consumed += take;

        while (s_enc_acc_len >= s_enc_frame_samples)
        {
            esp_audio_enc_in_frame_t in = {
                .buffer = (uint8_t *)s_enc_acc,
                .len = (uint32_t)(s_enc_frame_samples * sizeof(int16_t)),
            };
            esp_audio_enc_out_frame_t out = {
                .buffer = s_enc_out,
                .len = (uint32_t)s_enc_out_size,
                .encoded_bytes = 0,
            };

            if (esp_opus_enc_process(s_encoder, &in, &out) == ESP_AUDIO_ERR_OK &&
                out.encoded_bytes > 0)
            {
                if ((size_t)out.encoded_bytes > s_enc_out_size)
                {
                    /* Should be impossible now that the buffer is sized to
                     * the Opus maximum, but if it ever happens the heap is
                     * already damaged and silence is the worst response. */
                    ESP_LOGE(TAG,
                             "Opus wrote %u bytes into a %u byte buffer",
                             (unsigned)out.encoded_bytes,
                             (unsigned)s_enc_out_size);
                }
                else
                {
                    cb(s_enc_out, (size_t)out.encoded_bytes, user_ctx);
                }
            }
            else
            {
                ESP_LOGW(TAG, "Opus encode failed for one frame");
            }

            /* Shift the remainder down. */
            size_t leftover = s_enc_acc_len - s_enc_frame_samples;
            if (leftover > 0)
            {
                memmove(s_enc_acc, &s_enc_acc[s_enc_frame_samples],
                        leftover * sizeof(int16_t));
            }
            s_enc_acc_len = leftover;
        }
    }

    return ESP_OK;
}

esp_err_t koyoda_codec_decode(
    const uint8_t *opus_data,
    size_t opus_len,
    int16_t *out_pcm_22k,
    size_t out_capacity,
    size_t *out_samples)
{
    if (!s_open || opus_data == NULL || out_pcm_22k == NULL || out_samples == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    *out_samples = 0;
    if (opus_len == 0)
    {
        return ESP_OK;
    }

    /* 1. Opus -> 16 kHz PCM. */
    esp_audio_dec_in_raw_t raw = {
        .buffer = (uint8_t *)opus_data,
        .len = (uint32_t)opus_len,
        .consumed = 0,
        .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
    };
    esp_audio_dec_out_frame_t out_frame = {
        .buffer = (uint8_t *)s_dec_pcm16k,
        .len = (uint32_t)(KOYODA_OPUS_FRAME_SAMPLES * 2 * sizeof(int16_t)),
        .decoded_size = 0,
    };
    esp_audio_dec_info_t info;
    memset(&info, 0, sizeof(info));

    if (esp_opus_dec_decode(s_decoder, &raw, &out_frame, &info) != ESP_AUDIO_ERR_OK)
    {
        return ESP_FAIL;
    }

    size_t pcm16k_samples = out_frame.decoded_size / sizeof(int16_t);
    if (pcm16k_samples == 0)
    {
        return ESP_OK;
    }

    /* 2. 16 kHz -> 22050 for KOYODA's speaker path. */
    uint32_t max_out = 0;
    if (esp_ae_rate_cvt_get_max_out_sample_num(
            s_out_resampler, pcm16k_samples, &max_out) != ESP_AE_ERR_OK)
    {
        return ESP_FAIL;
    }
    if (max_out > out_capacity)
    {
        ESP_LOGW(TAG, "Decoded block needs %u samples, caller gave %u",
                 (unsigned)max_out, (unsigned)out_capacity);
        return ESP_ERR_NO_MEM;
    }

    uint32_t produced = max_out;
    if (esp_ae_rate_cvt_process(
            s_out_resampler,
            (esp_ae_sample_t)s_dec_pcm16k, (uint32_t)pcm16k_samples,
            (esp_ae_sample_t)out_pcm_22k, &produced) != ESP_AE_ERR_OK)
    {
        return ESP_FAIL;
    }

    *out_samples = produced;
    return ESP_OK;
}
