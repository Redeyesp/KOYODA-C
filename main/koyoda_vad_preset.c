#include "koyoda_vad_preset.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "KOYODA_VAD";

#define VAD_NVS_NAMESPACE "koyoda_vad"
#define VAD_NVS_KEY       "preset"

/*
 * NORMAL is first in intent even though it is second in the enum: it holds
 * the exact constants koyoda_audio_duplex.c shipped with, so a device that
 * has never been configured behaves identically to every earlier build.
 */
static const koyoda_vad_params_t k_presets[KOYODA_VAD_PRESET_COUNT] = {
    [KOYODA_VAD_PRESET_SENSITIVE] = {
        .start_frames     = 2,
        .end_silence_ms   = 1200,
        .min_start_level  = 50U,
        .noise_multiplier = 2U,
        .noise_margin     = 20U,
    },
    [KOYODA_VAD_PRESET_NORMAL] = {
        /* Original values. Do not change: this is the baseline every
         * earlier measurement was taken against. */
        .start_frames     = 3,
        .end_silence_ms   = 850,
        .min_start_level  = 70U,
        .noise_multiplier = 3U,
        .noise_margin     = 30U,
    },
    [KOYODA_VAD_PRESET_OUTDOOR] = {
        .start_frames     = 4,
        .end_silence_ms   = 700,
        .min_start_level  = 180U,
        .noise_multiplier = 5U,
        .noise_margin     = 80U,
    },
};

static const char *k_names[KOYODA_VAD_PRESET_COUNT] = {
    "SENSITIVE", "NORMAL", "OUTDOOR"
};

static const char *k_hints[KOYODA_VAD_PRESET_COUNT] = {
    "Quiet room, soft or slow speech",
    "Everyday use (tested default)",
    "Crowds, traffic, noisy places",
};

/* Starts as NORMAL so that even if init() is never reached, the audio task
 * finds the original values rather than zeros. */
koyoda_vad_params_t g_koyoda_vad = {
    .start_frames     = 3,
    .end_silence_ms   = 850,
    .min_start_level  = 70U,
    .noise_multiplier = 3U,
    .noise_margin     = 30U,
};

static koyoda_vad_preset_t s_current = KOYODA_VAD_PRESET_NORMAL;

static void apply_locked(koyoda_vad_preset_t preset)
{
    const koyoda_vad_params_t *p = &k_presets[preset];

    /* Field-by-field so the audio task always reads whole 32-bit words. */
    g_koyoda_vad.start_frames     = p->start_frames;
    g_koyoda_vad.end_silence_ms   = p->end_silence_ms;
    g_koyoda_vad.min_start_level  = p->min_start_level;
    g_koyoda_vad.noise_multiplier = p->noise_multiplier;
    g_koyoda_vad.noise_margin     = p->noise_margin;

    s_current = preset;

    ESP_LOGI(TAG,
             "VAD preset %s: end silence %u ms, threshold = noise*%u + %u, floor %u",
             k_names[preset],
             (unsigned)p->end_silence_ms,
             (unsigned)p->noise_multiplier,
             (unsigned)p->noise_margin,
             (unsigned)p->min_start_level);
}

void koyoda_vad_preset_init(void)
{
    koyoda_vad_preset_t preset = KOYODA_VAD_PRESET_NORMAL;

    nvs_handle_t handle;
    if (nvs_open(VAD_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK)
    {
        uint8_t stored = (uint8_t)KOYODA_VAD_PRESET_NORMAL;
        if (nvs_get_u8(handle, VAD_NVS_KEY, &stored) == ESP_OK &&
            stored < (uint8_t)KOYODA_VAD_PRESET_COUNT)
        {
            preset = (koyoda_vad_preset_t)stored;
        }
        nvs_close(handle);
    }

    apply_locked(preset);
}

koyoda_vad_preset_t koyoda_vad_preset_get(void)
{
    return s_current;
}

void koyoda_vad_preset_set(koyoda_vad_preset_t preset)
{
    if (preset >= KOYODA_VAD_PRESET_COUNT)
    {
        return;
    }
    if (preset == s_current)
    {
        return;
    }

    apply_locked(preset);

    nvs_handle_t handle;
    if (nvs_open(VAD_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK)
    {
        nvs_set_u8(handle, VAD_NVS_KEY, (uint8_t)preset);
        nvs_commit(handle);
        nvs_close(handle);
    }
    else
    {
        ESP_LOGW(TAG, "Could not persist preset; it will reset on reboot");
    }
}

const char *koyoda_vad_preset_name(koyoda_vad_preset_t preset)
{
    return (preset < KOYODA_VAD_PRESET_COUNT) ? k_names[preset] : "?";
}

const char *koyoda_vad_preset_hint(koyoda_vad_preset_t preset)
{
    return (preset < KOYODA_VAD_PRESET_COUNT) ? k_hints[preset] : "";
}
