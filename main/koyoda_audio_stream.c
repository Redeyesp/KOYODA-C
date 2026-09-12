#include "koyoda_audio_stream.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

#include "koyoda_audio_duplex.h"
#include "koyoda_face_state.h"

static const char *TAG = "KOYODA_STREAM";

#ifndef CONFIG_KOYODA_STREAM_HOST
#define CONFIG_KOYODA_STREAM_HOST "192.168.1.100"
#endif

#ifndef CONFIG_KOYODA_STREAM_PORT
#define CONFIG_KOYODA_STREAM_PORT 7777
#endif

#define STREAM_QUEUE_DEPTH 12
#define STREAM_PCM_SAMPLES_PER_MSG 256
#define STREAM_RX_MAX_BYTES (STREAM_PCM_SAMPLES_PER_MSG * sizeof(int16_t))

/* KOYODA -> PC microphone stream. */
#define STREAM_TYPE_START 1
#define STREAM_TYPE_PCM   2
#define STREAM_TYPE_END   3

/* PC -> KOYODA AI reply playback. */
#define STREAM_TYPE_PLAY_START 4
#define STREAM_TYPE_PLAY_PCM   5
#define STREAM_TYPE_PLAY_END   6
#define STREAM_TYPE_THINK_START 7
#define STREAM_TYPE_THINK_END   8

typedef struct
{
    uint8_t type;
    uint16_t sample_count;
    int16_t samples[STREAM_PCM_SAMPLES_PER_MSG];
} stream_msg_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_prev_vad = false;
static volatile uint32_t s_dropped_frames = 0;

static void enqueue_marker(uint8_t type)
{
    if (s_queue == NULL) return;

    stream_msg_t msg = {
        .type = type,
        .sample_count = 0,
    };

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE)
        s_dropped_frames++;
}

static void audio_frame_callback(
    const int16_t *samples,
    size_t sample_count,
    bool vad_speaking,
    void *user_ctx)
{
    (void)user_ctx;

    if (s_queue == NULL)
    {
        return;
    }

    if (!koyoda_audio_duplex_ai_is_enabled())
    {
        s_prev_vad = false;
        return;
    }

    bool prev = s_prev_vad;

    if (!prev && vad_speaking)
        enqueue_marker(STREAM_TYPE_START);

    if (vad_speaking)
    {
        stream_msg_t msg = {
            .type = STREAM_TYPE_PCM,
            .sample_count = 0,
        };

        size_t n = sample_count;
        if (n > STREAM_PCM_SAMPLES_PER_MSG)
            n = STREAM_PCM_SAMPLES_PER_MSG;

        memcpy(msg.samples, samples, n * sizeof(int16_t));
        msg.sample_count = (uint16_t)n;

        if (xQueueSend(s_queue, &msg, 0) != pdTRUE)
            s_dropped_frames++;
    }

    if (prev && !vad_speaking)
        enqueue_marker(STREAM_TYPE_END);

    s_prev_vad = vad_speaking;
}

static bool send_all(int sock, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < len)
    {
        int ret = send(sock, p + sent, len - sent, 0);
        if (ret <= 0) return false;
        sent += (size_t)ret;
    }
    return true;
}

static bool recv_all(int sock, void *data, size_t len)
{
    uint8_t *p = (uint8_t *)data;
    size_t received = 0;

    while (received < len)
    {
        int ret = recv(sock, p + received, len - received, 0);
        if (ret <= 0)
        {
            return false;
        }
        received += (size_t)ret;
    }

    return true;
}

static bool send_packet(
    int sock,
    uint8_t type,
    const void *payload,
    uint32_t payload_len)
{
    uint8_t header[8] = {
        'K','O','Y','A',
        type,
        (uint8_t)((payload_len >> 16) & 0xFF),
        (uint8_t)((payload_len >> 8) & 0xFF),
        (uint8_t)(payload_len & 0xFF)
    };

    if (!send_all(sock, header, sizeof(header))) return false;
    if (payload_len > 0)
        return send_all(sock, payload, payload_len);
    return true;
}

static bool receive_playback_packet(
    int sock,
    bool *playback_open)
{
    uint8_t header[8];

    if (!recv_all(sock, header, sizeof(header)))
    {
        return false;
    }

    if (memcmp(header, "KOYA", 4) != 0)
    {
        ESP_LOGE(TAG, "Invalid packet magic from PC");
        return false;
    }

    uint8_t type = header[4];
    uint32_t payload_len =
        ((uint32_t)header[5] << 16) |
        ((uint32_t)header[6] << 8) |
        (uint32_t)header[7];

    if (type == STREAM_TYPE_THINK_START)
    {
        if (payload_len != 0)
        {
            ESP_LOGE(TAG, "THINK_START payload must be empty");
            return false;
        }

        koyoda_face_state_set(KOYODA_FACE_AI_THINKING);
        ESP_LOGI(TAG, "AI THINK RX START");
        return true;
    }

    if (type == STREAM_TYPE_THINK_END)
    {
        if (payload_len != 0)
        {
            ESP_LOGE(TAG, "THINK_END payload must be empty");
            return false;
        }

        /* Do not let a late THINK_END override active speaker animation. */
        if (koyoda_face_state_get() == KOYODA_FACE_AI_THINKING)
        {
            koyoda_face_state_set(KOYODA_FACE_AI_IDLE);
        }
        ESP_LOGI(TAG, "AI THINK RX END");
        return true;
    }

    if (type == STREAM_TYPE_PLAY_START)
    {
        if (payload_len != 0)
        {
            ESP_LOGE(TAG, "PLAY_START payload must be empty");
            return false;
        }

        esp_err_t err = koyoda_audio_duplex_playback_start();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Could not start remote playback: %s", esp_err_to_name(err));
            return false;
        }

        *playback_open = true;
        /*
         * The audio-owner task now owns SPEAKING timing.
         * PLAY_START only queues playback; the face changes when the
         * ES8311 actually begins consuming the reply.
         */
        ESP_LOGI(TAG, "AI REPLY RX START");
        return true;
    }

    if (type == STREAM_TYPE_PLAY_PCM)
    {
        if (payload_len == 0 ||
            payload_len > STREAM_RX_MAX_BYTES ||
            (payload_len & 1U) != 0U)
        {
            ESP_LOGE(TAG, "Invalid PLAY_PCM length=%lu", (unsigned long)payload_len);
            return false;
        }

        int16_t samples[STREAM_PCM_SAMPLES_PER_MSG];

        if (!recv_all(sock, samples, payload_len))
        {
            return false;
        }

        esp_err_t err = koyoda_audio_duplex_playback_write(
            samples,
            payload_len / sizeof(int16_t));

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Could not queue remote PCM: %s", esp_err_to_name(err));
            return false;
        }

        return true;
    }

    if (type == STREAM_TYPE_PLAY_END)
    {
        if (payload_len != 0)
        {
            ESP_LOGE(TAG, "PLAY_END payload must be empty");
            return false;
        }

        esp_err_t err = koyoda_audio_duplex_playback_end();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Could not end remote playback: %s", esp_err_to_name(err));
            return false;
        }

        *playback_open = false;
        /*
         * Do not end the speaking face here. PLAY_END only enters the
         * audio queue; the audio-owner returns the face to IDLE after
         * the real speaker playback has finished.
         */
        ESP_LOGI(TAG, "AI REPLY RX END");
        return true;
    }

    ESP_LOGE(
        TAG,
        "Unexpected PC packet type=%u len=%lu",
        (unsigned)type,
        (unsigned long)payload_len);

    return false;
}

static int connect_receiver(void)
{
    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%d", CONFIG_KOYODA_STREAM_PORT);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;

    int gai = getaddrinfo(
        CONFIG_KOYODA_STREAM_HOST,
        port_text,
        &hints,
        &result);

    if (gai != 0 || result == NULL)
        return -1;

    int sock = socket(
        result->ai_family,
        result->ai_socktype,
        result->ai_protocol);

    if (sock < 0)
    {
        freeaddrinfo(result);
        return -1;
    }

    int ret = connect(sock, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

    if (ret != 0)
    {
        close(sock);
        return -1;
    }

    return sock;
}

static void close_stream_socket(
    int *sock,
    bool *playback_open)
{
    /* Any broken backend transaction must release the AI face state. */
    koyoda_face_state_set(KOYODA_FACE_AI_IDLE);

    if (*playback_open)
    {
        /* Unblock the audio owner if the network died mid-reply. */
        (void)koyoda_audio_duplex_playback_end();
        *playback_open = false;
    }

    if (*sock >= 0)
    {
        close(*sock);
        *sock = -1;
    }
}

static void stream_task(void *arg)
{
    (void)arg;

    ESP_LOGI(
        TAG,
        "STREAM AI-07 gated ready -> %s:%d (AI default OFF)",
        CONFIG_KOYODA_STREAM_HOST,
        CONFIG_KOYODA_STREAM_PORT);

    int sock = -1;
    bool playback_open = false;
    uint64_t utterance_bytes = 0;
    TickType_t utterance_start = 0;

    while (1)
    {
        stream_msg_t msg;

        /*
         * Explicit user gate.  When OFF:
         * - close any active PC/cloud socket,
         * - abort pending playback,
         * - discard stale utterance queue entries,
         * - keep the AI face at IDLE.
         */
        if (!koyoda_audio_duplex_ai_is_enabled())
        {
            if (sock >= 0 || playback_open)
            {
                ESP_LOGI(TAG, "AI MODE OFF: closing active AI stream");
                close_stream_socket(&sock, &playback_open);
            }

            if (s_queue != NULL)
            {
                xQueueReset(s_queue);
            }

            s_prev_vad = false;
            koyoda_face_state_set(KOYODA_FACE_AI_IDLE);

            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        TickType_t wait =
            (sock < 0)
                ? portMAX_DELAY
                : pdMS_TO_TICKS(5);

        BaseType_t got_msg = xQueueReceive(s_queue, &msg, wait);

        if (sock < 0)
        {
            /* Keep the original behavior: connect only when speech gives us
             * something useful to send. */
            if (got_msg != pdTRUE)
            {
                continue;
            }

            sock = connect_receiver();

            if (sock < 0)
            {
                if (msg.type == STREAM_TYPE_START)
                {
                    ESP_LOGW(
                        TAG,
                        "Receiver unavailable at %s:%d",
                        CONFIG_KOYODA_STREAM_HOST,
                        CONFIG_KOYODA_STREAM_PORT);
                }
                continue;
            }

            ESP_LOGI(TAG, "TCP connected to receiver (full duplex)");
        }

        if (got_msg == pdTRUE)
        {
            bool ok = true;

            if (msg.type == STREAM_TYPE_START)
            {
                utterance_bytes = 0;
                utterance_start = xTaskGetTickCount();
                ok = send_packet(sock, STREAM_TYPE_START, NULL, 0);
                ESP_LOGI(TAG, "VOICE STREAM START");
            }
            else if (msg.type == STREAM_TYPE_PCM)
            {
                uint32_t bytes =
                    (uint32_t)msg.sample_count * sizeof(int16_t);

                ok = send_packet(
                    sock,
                    STREAM_TYPE_PCM,
                    msg.samples,
                    bytes);

                if (ok)
                    utterance_bytes += bytes;
            }
            else if (msg.type == STREAM_TYPE_END)
            {
                ok = send_packet(sock, STREAM_TYPE_END, NULL, 0);

                uint32_t duration_ms =
                    (uint32_t)(
                        (xTaskGetTickCount() - utterance_start) *
                        portTICK_PERIOD_MS);

                ESP_LOGI(
                    TAG,
                    "VOICE STREAM END duration=%lums bytes=%llu dropped=%lu",
                    (unsigned long)duration_ms,
                    (unsigned long long)utterance_bytes,
                    (unsigned long)s_dropped_frames);
            }

            if (!ok)
            {
                ESP_LOGW(
                    TAG,
                    "Socket send failed errno=%d; closing connection",
                    errno);
                close_stream_socket(&sock, &playback_open);
                continue;
            }
        }

        if (sock >= 0)
        {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sock, &readfds);

            struct timeval tv = {
                .tv_sec = 0,
                .tv_usec = 0,
            };

            int ready = select(sock + 1, &readfds, NULL, NULL, &tv);

            if (ready < 0)
            {
                ESP_LOGW(TAG, "Socket select failed errno=%d", errno);
                close_stream_socket(&sock, &playback_open);
                continue;
            }

            if (ready > 0 && FD_ISSET(sock, &readfds))
            {
                if (!receive_playback_packet(sock, &playback_open))
                {
                    ESP_LOGW(
                        TAG,
                        "Reply receive failed/disconnected errno=%d; closing connection",
                        errno);
                    close_stream_socket(&sock, &playback_open);
                }
            }
        }
    }
}

esp_err_t koyoda_audio_stream_start(void)
{
    if (s_task != NULL)
        return ESP_OK;

    s_queue = xQueueCreate(
        STREAM_QUEUE_DEPTH,
        sizeof(stream_msg_t));

    if (s_queue == NULL)
        return ESP_ERR_NO_MEM;

    BaseType_t result = xTaskCreate(
        stream_task,
        "audio_stream",
        4096,
        NULL,
        2,
        &s_task);

    if (result != pdPASS)
    {
        vQueueDelete(s_queue);
        s_queue = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    koyoda_audio_duplex_set_frame_callback(
        audio_frame_callback,
        NULL);

    return ESP_OK;
}
