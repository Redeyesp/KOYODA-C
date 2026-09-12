#include "koyoda_codec_test.h"
#include "koyoda_codec.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "KOYODA_CODECTEST";

#define TEST_TONE_HZ        440
#define TEST_DURATION_MS    1000
#define TEST_AMPLITUDE      8000
#define TEST_IN_SAMPLES     ((KOYODA_PCM_SAMPLE_RATE * TEST_DURATION_MS) / 1000)
/* Generous: output can never legitimately exceed input by much. */
#define TEST_OUT_CAPACITY   (TEST_IN_SAMPLES * 2)
#define TEST_BLOCK          256

typedef struct
{
    int16_t *out;
    size_t out_len;
    size_t out_cap;
    size_t packets;
    size_t opus_bytes;
    int16_t scratch[KOYODA_CODEC_MAX_PCM_OUT];
} test_ctx_t;

/* Each encoded frame is decoded straight back, so the test exercises the
 * exact encode and decode paths the backend uses. */
static void on_packet(const uint8_t *data, size_t len, void *user_ctx)
{
    test_ctx_t *ctx = (test_ctx_t *)user_ctx;
    ctx->packets++;
    ctx->opus_bytes += len;

    size_t produced = 0;
    if (koyoda_codec_decode(data, len, ctx->scratch,
                            KOYODA_CODEC_MAX_PCM_OUT, &produced) != ESP_OK)
    {
        ESP_LOGW(TAG, "decode failed for packet %u", (unsigned)ctx->packets);
        return;
    }

    size_t space = ctx->out_cap - ctx->out_len;
    if (produced > space)
    {
        produced = space;
    }
    memcpy(&ctx->out[ctx->out_len], ctx->scratch, produced * sizeof(int16_t));
    ctx->out_len += produced;
}

static double rms_of(const int16_t *s, size_t n)
{
    if (n == 0)
    {
        return 0.0;
    }
    double acc = 0.0;
    for (size_t i = 0; i < n; i++)
    {
        acc += (double)s[i] * (double)s[i];
    }
    return sqrt(acc / (double)n);
}

/*
 * Estimate tone frequency from zero crossings. For a pure tone this is
 * accurate enough to tell 440 Hz from 319 Hz or 606 Hz, which is all we
 * need. A small amplitude gate avoids counting noise around silence.
 */
static double freq_of(const int16_t *s, size_t n, int sample_rate)
{
    if (n < 2)
    {
        return 0.0;
    }
    const int16_t gate = TEST_AMPLITUDE / 10;
    size_t crossings = 0;
    int last_sign = 0;
    size_t first = 0, last = 0;
    bool seen = false;

    for (size_t i = 0; i < n; i++)
    {
        int sign;
        if (s[i] > gate)      sign = 1;
        else if (s[i] < -gate) sign = -1;
        else                   continue;

        if (last_sign != 0 && sign != last_sign)
        {
            crossings++;
            if (!seen) { first = i; seen = true; }
            last = i;
        }
        last_sign = sign;
    }

    if (crossings < 2 || last <= first)
    {
        return 0.0;
    }
    /* crossings-1 half-periods span (last-first) samples. */
    double span_s = (double)(last - first) / (double)sample_rate;
    return ((double)(crossings - 1) / 2.0) / span_s;
}

esp_err_t koyoda_codec_selftest_run(void)
{
    ESP_LOGI(TAG, "==== Opus self-test starting (no server/mic needed) ====");

    esp_err_t err = koyoda_codec_open();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "RESULT: FAIL - codec would not open (%s)", esp_err_to_name(err));
        return err;
    }

    int16_t *in = heap_caps_malloc(TEST_IN_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    test_ctx_t *ctx = heap_caps_calloc(1, sizeof(test_ctx_t), MALLOC_CAP_SPIRAM);
    int16_t *out = heap_caps_malloc(TEST_OUT_CAPACITY * sizeof(int16_t), MALLOC_CAP_SPIRAM);

    if (in == NULL || ctx == NULL || out == NULL)
    {
        ESP_LOGE(TAG, "RESULT: FAIL - out of memory for test buffers");
        free(in); free(ctx); free(out);
        koyoda_codec_close();
        return ESP_ERR_NO_MEM;
    }

    ctx->out = out;
    ctx->out_cap = TEST_OUT_CAPACITY;

    for (size_t i = 0; i < TEST_IN_SAMPLES; i++)
    {
        double t = (double)i / (double)KOYODA_PCM_SAMPLE_RATE;
        in[i] = (int16_t)(TEST_AMPLITUDE * sin(2.0 * M_PI * TEST_TONE_HZ * t));
    }

    /* Feed it in the same 256-sample blocks the mic callback produces. */
    koyoda_codec_encode_reset();
    for (size_t off = 0; off < TEST_IN_SAMPLES; off += TEST_BLOCK)
    {
        size_t n = TEST_IN_SAMPLES - off;
        if (n > TEST_BLOCK) n = TEST_BLOCK;
        koyoda_codec_encode_push(&in[off], n, on_packet, ctx);
    }

    double in_rms = rms_of(in, TEST_IN_SAMPLES);
    double out_rms = rms_of(ctx->out, ctx->out_len);
    double in_f = freq_of(in, TEST_IN_SAMPLES, KOYODA_PCM_SAMPLE_RATE);
    double out_f = freq_of(ctx->out, ctx->out_len, KOYODA_PCM_SAMPLE_RATE);
    double len_ratio = (TEST_IN_SAMPLES > 0)
        ? (double)ctx->out_len / (double)TEST_IN_SAMPLES : 0.0;

    ESP_LOGI(TAG, "in : %u samples, rms %.0f, freq %.1f Hz",
             (unsigned)TEST_IN_SAMPLES, in_rms, in_f);
    ESP_LOGI(TAG, "out: %u samples, rms %.0f, freq %.1f Hz",
             (unsigned)ctx->out_len, out_rms, out_f);
    ESP_LOGI(TAG, "opus: %u packets, %u bytes total, %.1f kbps",
             (unsigned)ctx->packets, (unsigned)ctx->opus_bytes,
             (ctx->opus_bytes * 8.0) / (double)TEST_DURATION_MS);

    bool len_ok  = (len_ratio > 0.90 && len_ratio < 1.10);
    bool freq_ok = (out_f > TEST_TONE_HZ * 0.93 && out_f < TEST_TONE_HZ * 1.07);
    bool rms_ok  = (out_rms > in_rms * 0.30 && out_rms < in_rms * 2.00);

    ESP_LOGI(TAG, "  [%s] length  ratio %.3f (want ~1.00)",
             len_ok ? "PASS" : "FAIL", len_ratio);
    ESP_LOGI(TAG, "  [%s] pitch   %.1f Hz (want ~%d Hz)",
             freq_ok ? "PASS" : "FAIL", out_f, TEST_TONE_HZ);
    ESP_LOGI(TAG, "  [%s] level   rms %.0f vs %.0f",
             rms_ok ? "PASS" : "FAIL", out_rms, in_rms);

    if (!len_ok || !freq_ok)
    {
        /* Name the most probable cause rather than leaving it to guesswork. */
        double r16 = (double)KOYODA_OPUS_SAMPLE_RATE / (double)KOYODA_PCM_SAMPLE_RATE;
        if (out_f > 0 && fabs(out_f / TEST_TONE_HZ - r16) < 0.08)
        {
            ESP_LOGE(TAG, "HINT: output is ~0.73x - the 16000->22050 output "
                          "resample looks missing or bypassed.");
        }
        else if (out_f > 0 && fabs(out_f / TEST_TONE_HZ - (1.0 / r16)) < 0.10)
        {
            ESP_LOGE(TAG, "HINT: output is ~1.38x - the 22050->16000 input "
                          "resample looks missing, or the rates are swapped.");
        }
        else if (ctx->packets == 0)
        {
            ESP_LOGE(TAG, "HINT: no Opus packets were produced at all - the "
                          "encoder or the 960-sample accumulator is at fault.");
        }
    }

    bool pass = len_ok && freq_ok && rms_ok;
    ESP_LOGI(TAG, "==== RESULT: %s ====", pass ? "PASS" : "FAIL");

    free(in);
    free(out);
    free(ctx);
    koyoda_codec_close();

    return pass ? ESP_OK : ESP_FAIL;
}
