#include "koyoda_audio_duplex.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "nvs.h"

#include "bsp/esp-bsp.h"

#include "koyoda_face_state.h"

/*
 * KOYODA Shared Audio Step 1
 *
 * Why this exists:
 *   The previous KOYODA test initialized microphone and speaker from
 *   separate tasks.  Each device worked alone, but enabling both caused the
 *   display to freeze.
 *
 * This module gives the complete audio subsystem ONE owner:
 *
 *   1) initialize BSP duplex I2S ONCE
 *   2) create ES7210 + ES8311 codec handles
 *   3) open BOTH using exactly the same audio format
 *   4) keep both handles open
 *   5) service microphone capture and speaker event beeps from ONE task
 *
 * The short beep temporarily pauses mic reads for about 120 ms.  Immediately
 * after the beep, capture resumes.  This is intentional for the first stable
 * voice-assistant baseline and avoids concurrent access to the shared codec
 * data interface.
 */

static const char *TAG = "KOYODA_AUDIO";

#define AUDIO_SAMPLE_RATE_HZ          22050
#define AUDIO_BITS_PER_SAMPLE            16
#define AUDIO_CHANNELS                     1

#define MIC_SAMPLES_PER_READ             256
#define MIC_GAIN_DB                     24.0f
#define MIC_REPORT_MS                    1000

#define DEFAULT_VOLUME_PERCENT             90
#define BEEP_TONE_HZ                      660
#define BEEP_TONE_MS                      120
#define BEEP_CHUNK_SAMPLES                128

#define AUDIO_START_DELAY_MS             3000
#define AUDIO_TASK_STACK_BYTES           8192
#define AUDIO_TASK_PRIORITY                 2
#define AUDIO_TASK_CORE                     1

/* AI-06 remote TTS playback: keep chunks small and bounded. */
#define PLAYBACK_CHUNK_SAMPLES            256
#define PLAYBACK_QUEUE_DEPTH                4
#define PLAYBACK_WAIT_TIMEOUT_MS          3000

#define PLAYBACK_MSG_START                  1
#define PLAYBACK_MSG_PCM                    2
#define PLAYBACK_MSG_END                    3

typedef struct
{
    uint8_t type;
    uint16_t sample_count;
    int16_t samples[PLAYBACK_CHUNK_SAMPLES];
} playback_msg_t;

/* =========================================================
 * VAD Step 1
 *
 * Lightweight energy-based VAD. No extra task, no AI, no network streaming.
 *
 * Start:
 *   several consecutive frames must exceed the adaptive threshold.
 *
 * End:
 *   speech must remain below threshold for ~850 ms.
 *
 * The noise floor adapts only while idle, so normal room noise does not
 * continually lift the threshold during speech.
 * ========================================================= */
#define VAD_START_CONSECUTIVE_FRAMES         3
#define VAD_END_SILENCE_MS                 850
#define VAD_POST_BEEP_IGNORE_MS            250

/* Absolute safety floor for a quiet room. */
#define VAD_MIN_START_LEVEL                  70U

/* Adaptive threshold = noise_floor * 3 + margin. */
#define VAD_NOISE_MULTIPLIER                  3U
#define VAD_NOISE_MARGIN                     30U

/* Noise-floor IIR: 31/32 old + 1/32 new. */
#define VAD_NOISE_FILTER_SHIFT                5U

#define AUDIO_EVT_CHARGE_BEEP       (1UL << 0)
#define AUDIO_EVT_TEST_BEEP         (1UL << 1)
#define AUDIO_EVT_VOLUME_CHANGE     (1UL << 2)
#define AUDIO_EVT_AI_ON_BEEP        (1UL << 3)
#define AUDIO_EVT_AI_OFF_BEEP       (1UL << 4)

#define NVS_NAMESPACE "koyoda_audio"
#define NVS_KEY_VOLUME "volume"
#define NVS_KEY_VOLUME_SCHEMA "vol_schema"
#define VOLUME_SCHEMA_VERSION 2U

static TaskHandle_t s_audio_task = NULL;
static QueueHandle_t s_playback_queue = NULL;

static esp_codec_dev_handle_t s_mic = NULL;
static esp_codec_dev_handle_t s_speaker = NULL;

static volatile bool s_ready = false;
static volatile bool s_mic_running = false;
static volatile int s_volume_percent = DEFAULT_VOLUME_PERCENT;

/* Explicit user-controlled AI gate. Always OFF after boot. */
static volatile bool s_ai_enabled = false;

static koyoda_audio_frame_cb_t s_frame_callback = NULL;
static void *s_frame_callback_ctx = NULL;

/* VAD state. */
static volatile bool s_vad_speaking = false;
static uint32_t s_vad_noise_floor = 20U;
static unsigned s_vad_start_counter = 0U;
static TickType_t s_vad_last_voice_tick = 0;
static TickType_t s_vad_voice_start_tick = 0;
static TickType_t s_vad_ignore_until_tick = 0;

static int clamp_volume(int percent)
{
    if (percent < 0)
    {
        return 0;
    }

    if (percent > 100)
    {
        return 100;
    }

    return percent;
}

static int32_t abs_sample(int16_t sample)
{
    int32_t value = sample;
    return value < 0 ? -value : value;
}

static int level_to_percent(uint32_t level)
{
    if (level >= 12000U)
    {
        return 100;
    }

    return (int)((level * 100U) / 12000U);
}

static uint32_t vad_threshold(void)
{
    uint32_t threshold =
        (s_vad_noise_floor * VAD_NOISE_MULTIPLIER) +
        VAD_NOISE_MARGIN;

    if (threshold < VAD_MIN_START_LEVEL)
    {
        threshold = VAD_MIN_START_LEVEL;
    }

    return threshold;
}

static bool tick_before(TickType_t a, TickType_t b)
{
    /*
     * FreeRTOS ticks wrap. Signed subtraction keeps short relative comparisons
     * correct across wraparound.
     */
    return ((int32_t)(a - b)) < 0;
}

static void vad_reset_after_beep(void)
{
    TickType_t now = xTaskGetTickCount();

    s_vad_start_counter = 0U;
    s_vad_last_voice_tick = now;
    s_vad_ignore_until_tick =
        now + pdMS_TO_TICKS(VAD_POST_BEEP_IGNORE_MS);

    /*
     * If a beep happened while speech was active, end that VAD segment here
     * rather than letting the speaker tone become part of the user's speech.
     */
    if (s_vad_speaking)
    {
        uint32_t duration_ms =
            (uint32_t)((now - s_vad_voice_start_tick) * portTICK_PERIOD_MS);

        s_vad_speaking = false;

        ESP_LOGI(
            TAG,
            "VAD VOICE END duration=%lums reason=speaker",
            (unsigned long)duration_ms);
    }
}

static void vad_process_frame(
    uint32_t frame_avg,
    uint32_t frame_peak)
{
    TickType_t now = xTaskGetTickCount();

    if (tick_before(now, s_vad_ignore_until_tick))
    {
        return;
    }

    const uint32_t threshold = vad_threshold();

    /*
     * Use average energy as the primary signal. Peak is kept only for
     * diagnostics and does not by itself trigger speech, which avoids clicks
     * and one-sample spikes creating false starts.
     */
    const bool above = frame_avg >= threshold;

    if (!s_vad_speaking)
    {
        /*
         * Adapt room noise only while idle and only from frames that are below
         * the current speech threshold.
         */
        if (!above)
        {
            s_vad_noise_floor =
                ((s_vad_noise_floor * ((1U << VAD_NOISE_FILTER_SHIFT) - 1U)) +
                 frame_avg) >>
                VAD_NOISE_FILTER_SHIFT;

            if (s_vad_noise_floor < 5U)
            {
                s_vad_noise_floor = 5U;
            }

            s_vad_start_counter = 0U;
        }
        else
        {
            s_vad_start_counter++;

            if (s_vad_start_counter >= VAD_START_CONSECUTIVE_FRAMES)
            {
                s_vad_speaking = true;
                s_vad_voice_start_tick = now;
                s_vad_last_voice_tick = now;
                s_vad_start_counter = 0U;

                ESP_LOGI(
                    TAG,
                    "VAD VOICE START avg=%lu peak=%lu threshold=%lu noise=%lu",
                    (unsigned long)frame_avg,
                    (unsigned long)frame_peak,
                    (unsigned long)threshold,
                    (unsigned long)s_vad_noise_floor);
            }
        }

        return;
    }

    if (above)
    {
        s_vad_last_voice_tick = now;
        return;
    }

    const uint32_t silence_ms =
        (uint32_t)((now - s_vad_last_voice_tick) * portTICK_PERIOD_MS);

    if (silence_ms >= VAD_END_SILENCE_MS)
    {
        const uint32_t duration_ms =
            (uint32_t)((now - s_vad_voice_start_tick) * portTICK_PERIOD_MS);

        s_vad_speaking = false;
        s_vad_start_counter = 0U;

        ESP_LOGI(
            TAG,
            "VAD VOICE END duration=%lums noise=%lu threshold=%lu",
            (unsigned long)duration_ms,
            (unsigned long)s_vad_noise_floor,
            (unsigned long)threshold);
    }
}

static void log_memory(const char *where)
{
    ESP_LOGI(
        TAG,
        "%s: DMA free=%u largest=%u | internal free=%u largest=%u",
        where,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void load_volume_from_nvs(void)
{
    nvs_handle_t handle = 0;

    esp_err_t err = nvs_open(
        NVS_NAMESPACE,
        NVS_READWRITE,
        &handle);

    if (err != ESP_OK)
    {
        s_volume_percent = DEFAULT_VOLUME_PERCENT;
        ESP_LOGI(TAG, "Using default volume: %d%%", DEFAULT_VOLUME_PERCENT);
        return;
    }

    uint8_t schema = 0U;
    esp_err_t schema_err = nvs_get_u8(
        handle,
        NVS_KEY_VOLUME_SCHEMA,
        &schema);

    if (schema_err != ESP_OK || schema < VOLUME_SCHEMA_VERSION)
    {
        s_volume_percent = DEFAULT_VOLUME_PERCENT;

        esp_err_t write_err = nvs_set_u8(
            handle,
            NVS_KEY_VOLUME,
            (uint8_t)DEFAULT_VOLUME_PERCENT);

        if (write_err == ESP_OK)
        {
            write_err = nvs_set_u8(
                handle,
                NVS_KEY_VOLUME_SCHEMA,
                (uint8_t)VOLUME_SCHEMA_VERSION);
        }

        if (write_err == ESP_OK)
        {
            write_err = nvs_commit(handle);
        }

        nvs_close(handle);

        if (write_err == ESP_OK)
        {
            ESP_LOGI(TAG, "Volume default migrated to %d%%", DEFAULT_VOLUME_PERCENT);
        }
        else
        {
            ESP_LOGW(TAG, "Volume migration save failed: %s",
                     esp_err_to_name(write_err));
        }
        return;
    }

    uint8_t saved = DEFAULT_VOLUME_PERCENT;
    err = nvs_get_u8(handle, NVS_KEY_VOLUME, &saved);
    nvs_close(handle);

    if (err == ESP_OK)
    {
        s_volume_percent = clamp_volume((int)saved);
        ESP_LOGI(TAG, "Loaded saved volume: %d%%", (int)s_volume_percent);
    }
    else
    {
        s_volume_percent = DEFAULT_VOLUME_PERCENT;
        ESP_LOGI(TAG, "No saved volume; using default %d%%",
                 DEFAULT_VOLUME_PERCENT);
    }
}

static void save_volume_to_nvs(int percent)
{
    nvs_handle_t handle = 0;

    esp_err_t err = nvs_open(
        NVS_NAMESPACE,
        NVS_READWRITE,
        &handle);

    if (err != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Could not open NVS for volume: %s",
            esp_err_to_name(err));
        return;
    }

    err = nvs_set_u8(
        handle,
        NVS_KEY_VOLUME,
        (uint8_t)clamp_volume(percent));

    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Could not save volume: %s",
            esp_err_to_name(err));
    }
}

static void apply_volume(void)
{
    if (s_speaker == NULL)
    {
        return;
    }

    int volume = clamp_volume((int)s_volume_percent);

    int ret = esp_codec_dev_set_out_vol(
        s_speaker,
        volume);

    if (ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGW(
            TAG,
            "Speaker volume %d%% failed: %d",
            volume,
            ret);
    }
    else
    {
        ESP_LOGI(
            TAG,
            "Speaker volume -> %d%%",
            volume);
    }
}

static void fill_square_tone(
    int16_t *samples,
    size_t count,
    uint32_t *phase)
{
    uint32_t period =
        AUDIO_SAMPLE_RATE_HZ / BEEP_TONE_HZ;

    if (period < 2U)
    {
        period = 2U;
    }

    const int16_t amplitude = 3500;

    for (size_t i = 0; i < count; ++i)
    {
        uint32_t p = (*phase) % period;

        samples[i] =
            (p < (period / 2U))
                ? amplitude
                : (int16_t)-amplitude;

        (*phase)++;
    }
}

static bool play_one_beep(void)
{
    if (s_speaker == NULL)
    {
        return false;
    }

    int16_t samples[BEEP_CHUNK_SAMPLES];
    uint32_t phase = 0;

    const uint32_t total_samples =
        (AUDIO_SAMPLE_RATE_HZ * BEEP_TONE_MS) / 1000U;

    uint32_t sent = 0;

    while (sent < total_samples)
    {
        size_t chunk = BEEP_CHUNK_SAMPLES;
        uint32_t remaining = total_samples - sent;

        if (remaining < chunk)
        {
            chunk = remaining;
        }

        fill_square_tone(
            samples,
            chunk,
            &phase);

        int ret = esp_codec_dev_write(
            s_speaker,
            samples,
            chunk * sizeof(int16_t));

        if (ret != ESP_CODEC_DEV_OK)
        {
            ESP_LOGE(
                TAG,
                "Speaker write failed: %d",
                ret);
            return false;
        }

        sent += chunk;
        taskYIELD();
    }

    return true;
}

static void service_remote_playback_if_pending(void)
{
    if (s_playback_queue == NULL || s_speaker == NULL)
    {
        return;
    }

    if (!s_ai_enabled)
    {
        /*
         * Do not allow a stale cloud/PC reply to speak after the user has
         * explicitly turned AI OFF.
         */
        xQueueReset(s_playback_queue);
        koyoda_face_state_set(KOYODA_FACE_AI_IDLE);
        return;
    }

    playback_msg_t msg;

    if (xQueueReceive(s_playback_queue, &msg, 0) != pdTRUE)
    {
        return;
    }

    /* Ignore stale data until a fresh START marker arrives. */
    if (msg.type != PLAYBACK_MSG_START)
    {
        ESP_LOGW(TAG, "Dropping remote playback packet before START");
        return;
    }

    /*
     * Tie the speaking face to REAL speaker playback, not to network
     * packet arrival. This keeps mouth animation alive for the complete
     * audible reply and removes the intermittent no-mouth-movement case.
     */
    koyoda_face_state_set(KOYODA_FACE_AI_SPEAKING);
    ESP_LOGI(TAG, "REMOTE SPEAK START: mic/VAD paused");

    s_mic_running = false;
    vad_reset_after_beep();

    uint64_t total_samples = 0;
    TickType_t started = xTaskGetTickCount();
    bool finished = false;

    while (!finished)
    {
        if (!s_ai_enabled)
        {
            ESP_LOGI(TAG, "REMOTE SPEAK aborted: AI MODE OFF");
            xQueueReset(s_playback_queue);
            break;
        }

        if (xQueueReceive(
                s_playback_queue,
                &msg,
                pdMS_TO_TICKS(PLAYBACK_WAIT_TIMEOUT_MS)) != pdTRUE)
        {
            ESP_LOGW(
                TAG,
                "REMOTE SPEAK timeout waiting for PCM/END; aborting");
            break;
        }

        if (msg.type == PLAYBACK_MSG_PCM)
        {
            if (msg.sample_count == 0)
            {
                continue;
            }

            int ret = esp_codec_dev_write(
                s_speaker,
                msg.samples,
                (size_t)msg.sample_count * sizeof(int16_t));

            if (ret != ESP_CODEC_DEV_OK)
            {
                ESP_LOGE(TAG, "Remote speaker write failed: %d", ret);
                break;
            }

            total_samples += msg.sample_count;
        }
        else if (msg.type == PLAYBACK_MSG_END)
        {
            finished = true;
        }
        else if (msg.type == PLAYBACK_MSG_START)
        {
            /* A repeated START restarts timing but does not touch the codec. */
            started = xTaskGetTickCount();
            total_samples = 0;
        }
    }

    vad_reset_after_beep();
    s_mic_running = true;
    koyoda_face_state_set(KOYODA_FACE_AI_IDLE);

    uint32_t duration_ms =
        (uint32_t)((xTaskGetTickCount() - started) * portTICK_PERIOD_MS);

    ESP_LOGI(
        TAG,
        "REMOTE SPEAK END duration=%lums samples=%llu; mic/VAD resumed",
        (unsigned long)duration_ms,
        (unsigned long long)total_samples);
}

static bool initialize_shared_audio(void)
{
    log_memory("before shared audio init");

    /*
     * IMPORTANT:
     * This is the ONLY explicit BSP audio initialization call in KOYODA.
     * With NULL, the Waveshare BSP uses its duplex
     * 22050 Hz / 16-bit / mono profile.
     */
    ESP_LOGI(
        TAG,
        "PHASE 1/6: bsp_audio_init(NULL) ONCE");

    esp_err_t err = bsp_audio_init(NULL);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "bsp_audio_init failed: %s",
            esp_err_to_name(err));
        return false;
    }

    log_memory("after I2S duplex init");

    ESP_LOGI(
        TAG,
        "PHASE 2/6: creating ES7210 microphone");

    s_mic = bsp_audio_codec_microphone_init();

    if (s_mic == NULL)
    {
        ESP_LOGE(
            TAG,
            "bsp_audio_codec_microphone_init returned NULL");
        return false;
    }

    ESP_LOGI(
        TAG,
        "PHASE 3/6: creating ES8311 speaker");

    s_speaker = bsp_audio_codec_speaker_init();

    if (s_speaker == NULL)
    {
        ESP_LOGE(
            TAG,
            "bsp_audio_codec_speaker_init returned NULL");
        return false;
    }

    esp_codec_dev_sample_info_t format = {
        .sample_rate = AUDIO_SAMPLE_RATE_HZ,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
    };

    /*
     * Open both BEFORE any continuous mic read loop starts.
     * Both use exactly the same sample format.
     */
    ESP_LOGI(
        TAG,
        "PHASE 4/6: opening ES7210 capture");

    int ret = esp_codec_dev_open(
        s_mic,
        &format);

    if (ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGE(
            TAG,
            "Mic open failed: %d",
            ret);
        return false;
    }

    ret = esp_codec_dev_set_in_gain(
        s_mic,
        MIC_GAIN_DB);

    if (ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGW(
            TAG,
            "Mic gain %.1f dB not accepted: %d",
            MIC_GAIN_DB,
            ret);
    }

    ESP_LOGI(
        TAG,
        "PHASE 5/6: opening ES8311 playback");

    ret = esp_codec_dev_open(
        s_speaker,
        &format);

    if (ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGE(
            TAG,
            "Speaker open failed: %d",
            ret);
        return false;
    }

    apply_volume();

    log_memory("after both codecs open");

    ESP_LOGI(
        TAG,
        "PHASE 6/6: SHARED AUDIO READY 22050 Hz / 16-bit / mono");

    s_ready = true;

    return true;
}

static void service_events(uint32_t events)
{
    if (events & AUDIO_EVT_VOLUME_CHANGE)
    {
        int volume =
            clamp_volume((int)s_volume_percent);

        apply_volume();
        save_volume_to_nvs(volume);
    }

    if (events & AUDIO_EVT_CHARGE_BEEP)
    {
        ESP_LOGI(
            TAG,
            "CHARGE BEEP at %d%%; mic pauses ~%d ms",
            (int)s_volume_percent,
            BEEP_TONE_MS);

        play_one_beep();
        vad_reset_after_beep();
    }

    if (events & AUDIO_EVT_TEST_BEEP)
    {
        ESP_LOGI(
            TAG,
            "TEST BEEP at %d%%; mic pauses ~%d ms",
            (int)s_volume_percent,
            BEEP_TONE_MS);

        play_one_beep();
        vad_reset_after_beep();
    }

    if (events & AUDIO_EVT_AI_ON_BEEP)
    {
        ESP_LOGI(TAG, "AI ON confirmation: one beep");
        play_one_beep();
        vad_reset_after_beep();
    }

    if (events & AUDIO_EVT_AI_OFF_BEEP)
    {
        ESP_LOGI(TAG, "AI OFF confirmation: two beeps");
        play_one_beep();
        vTaskDelay(pdMS_TO_TICKS(90));
        play_one_beep();
        vad_reset_after_beep();
    }
}

static void audio_owner_task(void *arg)
{
    (void)arg;

    ESP_LOGI(
        TAG,
        "Shared audio owner pinned to CPU%d; init in %d ms",
        AUDIO_TASK_CORE,
        AUDIO_START_DELAY_MS);

    vTaskDelay(
        pdMS_TO_TICKS(AUDIO_START_DELAY_MS));

    if (!initialize_shared_audio())
    {
        ESP_LOGE(
            TAG,
            "Shared audio initialization failed; task stopping");

        s_ready = false;
        s_mic_running = false;
        s_audio_task = NULL;

        vTaskDelete(NULL);
        return;
    }

    /*
     * Boot beep happens only once, after BOTH codecs are already open.
     * After the short beep we immediately enter continuous mic capture.
     */
    ESP_LOGI(
        TAG,
        "BOOT BEEP ONCE at %d%%",
        (int)s_volume_percent);

    play_one_beep();
    vad_reset_after_beep();

    int16_t samples[MIC_SAMPLES_PER_READ];

    uint64_t sum = 0;
    uint32_t count = 0;
    uint32_t peak = 0;

    TickType_t last_report =
        xTaskGetTickCount();

    s_mic_running = true;

    ESP_LOGI(
        TAG,
        "MIC RUNNING; ES7210 + ES8311 stay open together");

    ESP_LOGI(
        TAG,
        "VAD READY: start=%u frames, end=%ums, adaptive noise floor",
        (unsigned)VAD_START_CONSECUTIVE_FRAMES,
        (unsigned)VAD_END_SILENCE_MS);

    while (1)
    {
        /*
         * Process pending speaker/volume events between mic reads.
         * No second task touches the shared audio data interface.
         */
        uint32_t events = 0;

        xTaskNotifyWait(
            0,
            UINT32_MAX,
            &events,
            0);

        if (events != 0)
        {
            service_events(events);
        }

        /*
         * Remote TTS playback is serviced by this same owner task.
         * While it runs, no microphone reads occur, giving us half-duplex
         * audio and preventing KOYODA from hearing its own generated voice.
         */
        service_remote_playback_if_pending();

        int ret = esp_codec_dev_read(
            s_mic,
            samples,
            sizeof(samples));

        if (ret != ESP_CODEC_DEV_OK)
        {
            ESP_LOGE(
                TAG,
                "Microphone read failed: %d",
                ret);

            vTaskDelay(
                pdMS_TO_TICKS(100));
            continue;
        }

        /*
         * AI OFF:
         * Keep draining the codec/I2S input for hardware stability, but do not
         * inspect sample energy, run VAD, call the stream callback, or send
         * anything over the network.
         */
        if (!s_ai_enabled)
        {
            s_vad_speaking = false;
            s_vad_start_counter = 0U;

            TickType_t now =
                xTaskGetTickCount();

            if ((now - last_report) >=
                pdMS_TO_TICKS(MIC_REPORT_MS))
            {
                ESP_LOGI(
                    TAG,
                    "AI OFF: mic frames discarded locally; VAD/network disabled");
                last_report = now;
            }

            vTaskDelay(
                pdMS_TO_TICKS(5));
            continue;
        }

        uint64_t frame_sum = 0;
        uint32_t frame_peak = 0;

        for (size_t i = 0;
             i < MIC_SAMPLES_PER_READ;
             ++i)
        {
            uint32_t level =
                (uint32_t)abs_sample(samples[i]);

            frame_sum += level;

            sum += level;
            count++;

            if (level > frame_peak)
            {
                frame_peak = level;
            }

            if (level > peak)
            {
                peak = level;
            }
        }

        const uint32_t frame_avg =
            (uint32_t)(frame_sum / MIC_SAMPLES_PER_READ);

        vad_process_frame(
            frame_avg,
            frame_peak);

        koyoda_audio_frame_cb_t frame_cb = s_frame_callback;
        if (frame_cb != NULL)
        {
            frame_cb(
                samples,
                MIC_SAMPLES_PER_READ,
                s_vad_speaking,
                s_frame_callback_ctx);
        }

        /*
         * Keep the CPU1 task cooperative.
         * This matches the stable mic-probe behavior.
         */
        vTaskDelay(
            pdMS_TO_TICKS(5));

        TickType_t now =
            xTaskGetTickCount();

        if ((now - last_report) >=
            pdMS_TO_TICKS(MIC_REPORT_MS))
        {
            uint32_t avg =
                count
                    ? (uint32_t)(sum / count)
                    : 0;

            ESP_LOGI(
                TAG,
                "MIC %3d%% avg=%5lu peak=%5lu | VAD=%s noise=%lu th=%lu",
                level_to_percent(avg),
                (unsigned long)avg,
                (unsigned long)peak,
                s_vad_speaking ? "VOICE" : "IDLE",
                (unsigned long)s_vad_noise_floor,
                (unsigned long)vad_threshold());

            sum = 0;
            count = 0;
            peak = 0;
            last_report = now;
        }
    }
}

esp_err_t koyoda_audio_duplex_start(void)
{
    if (s_audio_task != NULL)
    {
        return ESP_OK;
    }

    load_volume_from_nvs();

    if (s_playback_queue == NULL)
    {
        s_playback_queue = xQueueCreate(
            PLAYBACK_QUEUE_DEPTH,
            sizeof(playback_msg_t));

        if (s_playback_queue == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t result =
        xTaskCreatePinnedToCore(
            audio_owner_task,
            "audio_owner",
            AUDIO_TASK_STACK_BYTES,
            NULL,
            AUDIO_TASK_PRIORITY,
            &s_audio_task,
            AUDIO_TASK_CORE);

    if (result != pdPASS)
    {
        s_audio_task = NULL;

        if (s_playback_queue != NULL)
        {
            vQueueDelete(s_playback_queue);
            s_playback_queue = NULL;
        }

        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool koyoda_audio_duplex_is_ready(void)
{
    return s_ready;
}

bool koyoda_audio_duplex_mic_is_running(void)
{
    return s_mic_running;
}

bool koyoda_audio_duplex_vad_is_speaking(void)
{
    return s_vad_speaking;
}

void koyoda_audio_duplex_set_frame_callback(
    koyoda_audio_frame_cb_t callback,
    void *user_ctx)
{
    s_frame_callback_ctx = user_ctx;
    s_frame_callback = callback;
}

void koyoda_audio_duplex_ai_set_enabled(bool enabled)
{
    const bool previous = s_ai_enabled;

    if (previous == enabled)
    {
        return;
    }

    s_ai_enabled = enabled;

    /*
     * Always start a newly-enabled listening session from clean VAD state.
     * We intentionally do not persist this mode: every reboot resets OFF.
     */
    s_vad_speaking = false;
    s_vad_start_counter = 0U;
    s_vad_noise_floor = 20U;
    s_vad_last_voice_tick = xTaskGetTickCount();
    s_vad_voice_start_tick = 0;
    s_vad_ignore_until_tick =
        xTaskGetTickCount() + pdMS_TO_TICKS(VAD_POST_BEEP_IGNORE_MS);

    if (!enabled)
    {
        koyoda_face_state_set(KOYODA_FACE_AI_IDLE);

        if (s_playback_queue != NULL)
        {
            xQueueReset(s_playback_queue);
        }
    }

    TaskHandle_t task = s_audio_task;

    if (task != NULL)
    {
        xTaskNotify(
            task,
            enabled
                ? AUDIO_EVT_AI_ON_BEEP
                : AUDIO_EVT_AI_OFF_BEEP,
            eSetBits);
    }

    ESP_LOGI(
        TAG,
        "AI MODE %s: %s",
        enabled ? "ON" : "OFF",
        enabled
            ? "VAD + frame streaming enabled"
            : "mic frames discarded; VAD + streaming disabled");
}

bool koyoda_audio_duplex_ai_is_enabled(void)
{
    return s_ai_enabled;
}

void koyoda_audio_duplex_beep_charge(void)
{
    TaskHandle_t task = s_audio_task;

    if (task != NULL)
    {
        xTaskNotify(
            task,
            AUDIO_EVT_CHARGE_BEEP,
            eSetBits);
    }
}

void koyoda_audio_duplex_beep_test(void)
{
    TaskHandle_t task = s_audio_task;

    if (task != NULL)
    {
        xTaskNotify(
            task,
            AUDIO_EVT_TEST_BEEP,
            eSetBits);
    }
}

static esp_err_t playback_enqueue(
    const playback_msg_t *msg,
    TickType_t wait_ticks)
{
    if (s_playback_queue == NULL || s_audio_task == NULL || !s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_playback_queue, msg, wait_ticks) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t koyoda_audio_duplex_playback_start(void)
{
    playback_msg_t msg = {
        .type = PLAYBACK_MSG_START,
        .sample_count = 0,
    };

    return playback_enqueue(&msg, pdMS_TO_TICKS(1000));
}

esp_err_t koyoda_audio_duplex_playback_write(
    const int16_t *samples,
    size_t sample_count)
{
    if (samples == NULL || sample_count == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    while (sample_count > 0)
    {
        size_t n = sample_count;
        if (n > PLAYBACK_CHUNK_SAMPLES)
        {
            n = PLAYBACK_CHUNK_SAMPLES;
        }

        playback_msg_t msg = {
            .type = PLAYBACK_MSG_PCM,
            .sample_count = (uint16_t)n,
        };

        memcpy(msg.samples, samples, n * sizeof(int16_t));

        esp_err_t err = playback_enqueue(
            &msg,
            pdMS_TO_TICKS(1000));

        if (err != ESP_OK)
        {
            return err;
        }

        samples += n;
        sample_count -= n;
    }

    return ESP_OK;
}

esp_err_t koyoda_audio_duplex_playback_end(void)
{
    playback_msg_t msg = {
        .type = PLAYBACK_MSG_END,
        .sample_count = 0,
    };

    return playback_enqueue(&msg, pdMS_TO_TICKS(1000));
}

int koyoda_audio_duplex_get_volume(void)
{
    return clamp_volume(
        (int)s_volume_percent);
}

void koyoda_audio_duplex_set_volume(int percent)
{
    s_volume_percent =
        clamp_volume(percent);

    TaskHandle_t task = s_audio_task;

    if (task != NULL)
    {
        xTaskNotify(
            task,
            AUDIO_EVT_VOLUME_CHANGE,
            eSetBits);
    }
}
