#include "koyoda_backend.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_app_format.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_flash.h"
#include <sys/time.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

#include "koyoda_wifi.h"
#include "koyoda_audio_duplex.h"
#include "koyoda_face_state.h"
#include "koyoda_codec.h"
#include "koyoda_codec_test.h"

static const char *TAG = "KOYODA_BACKEND";

/* ------------------------------------------------------------------------- */
/* Configuration (see main/Kconfig.projbuild, menu "KOYODA Xiaozhi Backend") */
/* ------------------------------------------------------------------------- */

#ifndef CONFIG_KOYODA_OTA_URL
#define CONFIG_KOYODA_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"
#endif

#ifndef CONFIG_KOYODA_LANGUAGE
#define CONFIG_KOYODA_LANGUAGE "en-US"
#endif

#ifndef CONFIG_KOYODA_BOARD_NAME
#define CONFIG_KOYODA_BOARD_NAME "koyoda-esp32s3-amoled-1.75"
#endif

#define BACKEND_NVS_NAMESPACE   "koyoda_be"
#define BACKEND_NVS_UUID_KEY    "uuid"
#define BACKEND_NVS_WS_URL_KEY  "ws_url"
#define BACKEND_NVS_WS_TOK_KEY  "ws_token"
#define BACKEND_NVS_WS_VER_KEY  "ws_ver"

/* Protocol version 1 == raw payload, no binary framing header. Matches
 * xiaozhi-esp32's WebsocketProtocol default. */
#define KOYODA_PROTOCOL_VERSION 1

/* KOYODA's shared audio path runs at 22050 Hz mono 16-bit (see
 * koyoda_audio_duplex.h). We advertise that honestly instead of lying
 * about 16 kHz Opus we don't yet produce. See the codec-gap note in
 * koyoda_backend.h. */
#define KOYODA_AUDIO_SAMPLE_RATE 22050
#define KOYODA_AUDIO_FRAME_MS    12

#define BACKEND_MIC_QUEUE_DEPTH   8
#define BACKEND_MIC_SAMPLES_MAX   256
#define BACKEND_TICK_MS           500
#define BACKEND_ACTIVE_TICK_MS    20
#define BACKEND_OTA_RETRY_MS      (30U * 1000U)
#define BACKEND_WS_HELLO_TIMEOUT_MS 10000

/*
 * KOYODA owns the display from app_main, which runs at priority 1 pinned
 * to CPU0. This task MUST stay below/off that path or the face animation
 * visibly stutters:
 *   - priority 2 matches the audio owner and the old stream module,
 *   - CPU1 keeps LVGL flushes on CPU0 uncontended.
 */
#define BACKEND_TASK_PRIORITY     2
#define BACKEND_TASK_CORE         1
#define BACKEND_TASK_STACK_BYTES  6144

typedef struct
{
    uint16_t sample_count;
    int16_t samples[BACKEND_MIC_SAMPLES_MAX];
} mic_frame_msg_t;

/*
 * Control events raised by the audio task. The audio owner task must stay
 * deterministic, so it NEVER calls into the network stack directly -- it
 * only posts one of these and returns immediately. The backend worker
 * task is what actually sends them.
 */
typedef enum
{
    BE_CTRL_LISTEN_START = 0,
    BE_CTRL_LISTEN_STOP,
} backend_ctrl_t;

typedef enum
{
    BE_STATE_WIFI_DOWN = 0,
    BE_STATE_WIFI_UP,
    BE_STATE_OTA_CHECKIN,
    BE_STATE_WS_CONNECTING,
    BE_STATE_WS_OPEN,
    BE_STATE_BACKOFF,
} backend_state_t;

static volatile bool s_wifi_connected = false;
static volatile bool s_reconnect_requested = false;
static volatile backend_state_t s_state = BE_STATE_WIFI_DOWN;
static volatile uint32_t s_backoff_until_ms = 0;

static char s_uuid[37] = {0};
static char s_mac_str[18] = {0};
static char s_ws_url[192] = {0};
static char s_ws_token[128] = {0};
static int  s_ws_version = KOYODA_PROTOCOL_VERSION;
static bool s_activation_pending = false;
static char s_activation_code[32] = {0};
static char s_activation_message[128] = {0};
static uint32_t s_flash_size = 0;

static QueueHandle_t s_mic_queue = NULL;
static QueueHandle_t s_ctrl_queue = NULL;
static uint8_t *s_mic_queue_storage = NULL;
static StaticQueue_t s_mic_queue_struct;
static TaskHandle_t s_task = NULL;
static esp_websocket_client_handle_t s_ws = NULL;
static EventGroupHandle_t s_events = NULL;
#define WS_HELLO_BIT BIT0
#define WS_CONNECTED_BIT BIT1

static volatile bool s_channel_open = false;
static volatile bool s_playback_active = false;
static volatile bool s_prev_vad = false;

/* Scratch for decoded playback PCM. Only the websocket event task writes
 * it, and only one decode is ever in flight. */
static int16_t *s_playback_pcm = NULL;

#if CONFIG_KOYODA_ECHO_TEST
/*
 * Local echo test: mic -> resample -> Opus encode -> Opus decode ->
 * resample -> speaker, with no network involved at all.
 *
 * koyoda_audio_duplex is half-duplex (mic pauses during playback), so a
 * live echo is impossible. Instead we buffer the round-tripped audio
 * while VAD says you are speaking, then replay it when you stop. Speak,
 * pause, and you should hear yourself back at normal pitch and speed.
 */
#define ECHO_BUFFER_SECONDS 3
#define ECHO_BUFFER_SAMPLES (KOYODA_PCM_SAMPLE_RATE * ECHO_BUFFER_SECONDS)
static int16_t *s_echo_buf = NULL;
static size_t s_echo_len = 0;
static bool s_echo_active = false;
#endif

/* ------------------------------------------------------------------------- */
/* Device identity: MAC string + a persistent random UUID, mirroring        */
/* xiaozhi-esp32's SystemInfo::GetMacAddress() / Board::GetUuid().          */
/* ------------------------------------------------------------------------- */

static void load_or_create_identity(void)
{
    if (esp_flash_get_size(NULL, &s_flash_size) != ESP_OK)
    {
        s_flash_size = 0;
    }

    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_mac_str, sizeof(s_mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    nvs_handle_t handle;
    if (nvs_open(BACKEND_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    {
        /* Fall back to a MAC-derived id for this boot only. */
        snprintf(s_uuid, sizeof(s_uuid), "koyoda-%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return;
    }

    size_t len = sizeof(s_uuid);
    if (nvs_get_str(handle, BACKEND_NVS_UUID_KEY, s_uuid, &len) != ESP_OK)
    {
        uint8_t r[16];
        esp_fill_random(r, sizeof(r));
        r[6] = (r[6] & 0x0F) | 0x40; /* version 4 */
        r[8] = (r[8] & 0x3F) | 0x80; /* variant 1 */
        snprintf(s_uuid, sizeof(s_uuid),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                 r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
        nvs_set_str(handle, BACKEND_NVS_UUID_KEY, s_uuid);
        nvs_commit(handle);
    }

    /* Reuse a previously learned websocket target so KOYODA can open the
     * audio channel again even if a later OTA check-in fails (e.g. no
     * internet route to the OTA host but the LAN media server is fine). */
    len = sizeof(s_ws_url);
    nvs_get_str(handle, BACKEND_NVS_WS_URL_KEY, s_ws_url, &len);
    len = sizeof(s_ws_token);
    nvs_get_str(handle, BACKEND_NVS_WS_TOK_KEY, s_ws_token, &len);
    int32_t stored_ver = 0;
    if (nvs_get_i32(handle, BACKEND_NVS_WS_VER_KEY, &stored_ver) == ESP_OK &&
        stored_ver != 0)
    {
        s_ws_version = (int)stored_ver;
    }

    nvs_close(handle);
}

static void save_ws_target(const char *url, const char *token, int version)
{
    nvs_handle_t handle;
    if (nvs_open(BACKEND_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    {
        return;
    }
    if (url != NULL)
    {
        nvs_set_str(handle, BACKEND_NVS_WS_URL_KEY, url);
    }
    if (token != NULL)
    {
        nvs_set_str(handle, BACKEND_NVS_WS_TOK_KEY, token);
    }
    nvs_set_i32(handle, BACKEND_NVS_WS_VER_KEY, (int32_t)version);
    nvs_commit(handle);
    nvs_close(handle);
}

/* ------------------------------------------------------------------------- */
/* OTA check-in.                                                             */
/*                                                                            */
/* POSTs a small device-info JSON to CONFIG_KOYODA_OTA_URL, exactly the      */
/* shape xiaozhi-esp32's Ota::CheckVersion() expects a server to answer:     */
/*   { "websocket": {"url": "...", "token": "..."},                         */
/*     "firmware":  {"version": "...", "url": "...", "force": 0} }          */
/* Only the "websocket" section is consumed today. "firmware" is logged so  */
/* real OTA flashing can be added later (esp_https_ota is already linked).  */
/* ------------------------------------------------------------------------- */

/*
 * Parses all five sections xiaozhi-esp32's Ota::CheckVersion() handles:
 * activation, mqtt, websocket, server_time and firmware.
 * Returns ESP_OK only when a usable websocket target was obtained.
 */
static esp_err_t parse_checkin_response(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL)
    {
        ESP_LOGW(TAG, "Check-in response was not valid JSON");
        return ESP_FAIL;
    }

    esp_err_t result = ESP_FAIL;

    /* ---- activation: device not yet bound to an account ---- */
    s_activation_pending = false;
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation))
    {
        cJSON *code = cJSON_GetObjectItem(activation, "code");
        cJSON *message = cJSON_GetObjectItem(activation, "message");
        cJSON *challenge = cJSON_GetObjectItem(activation, "challenge");

        if (cJSON_IsString(code))
        {
            strlcpy(s_activation_code, code->valuestring, sizeof(s_activation_code));
            s_activation_pending = true;
        }
        if (cJSON_IsString(message))
        {
            strlcpy(s_activation_message, message->valuestring,
                    sizeof(s_activation_message));
        }

        if (s_activation_pending)
        {
            ESP_LOGW(TAG, "==============================================");
            ESP_LOGW(TAG, " DEVICE NOT ACTIVATED");
            ESP_LOGW(TAG, " Activation code : %s", s_activation_code);
            if (s_activation_message[0] != '\0')
            {
                ESP_LOGW(TAG, " Server message  : %s", s_activation_message);
            }
            ESP_LOGW(TAG, " Enter this code in the xiaozhi console, then");
            ESP_LOGW(TAG, " KOYODA will retry automatically.");
            ESP_LOGW(TAG, "==============================================");
        }
        if (cJSON_IsString(challenge))
        {
            /*
             * Challenge-response activation (Activation-Version 2) needs
             * an HMAC over a per-device secret burned into eFuse, which
             * KOYODA has no provisioning flow for. We advertise
             * Activation-Version 1 so servers should not send this; log
             * it rather than pretend we handled it.
             */
            ESP_LOGW(TAG, "Server sent an activation challenge; "
                          "KOYODA only supports Activation-Version 1");
        }
    }

    /* ---- mqtt: parsed and logged only; KOYODA uses the websocket
     * transport. Recorded so a mis-provisioned server is obvious. ---- */
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt))
    {
        cJSON *endpoint = cJSON_GetObjectItem(mqtt, "endpoint");
        ESP_LOGI(TAG, "Server offered MQTT (%s); KOYODA uses websocket",
                 cJSON_IsString(endpoint) ? endpoint->valuestring : "no endpoint");
    }

    /* ---- websocket: url, token and protocol version ---- */
    cJSON *ws = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(ws))
    {
        cJSON *url = cJSON_GetObjectItem(ws, "url");
        cJSON *token = cJSON_GetObjectItem(ws, "token");
        cJSON *ver = cJSON_GetObjectItem(ws, "version");

        if (cJSON_IsString(url))
        {
            strlcpy(s_ws_url, url->valuestring, sizeof(s_ws_url));
            strlcpy(s_ws_token,
                    cJSON_IsString(token) ? token->valuestring : "",
                    sizeof(s_ws_token));

            if (cJSON_IsNumber(ver) && ver->valueint != 0)
            {
                s_ws_version = ver->valueint;
            }

            save_ws_target(s_ws_url, s_ws_token, s_ws_version);
            ESP_LOGI(TAG, "Websocket target: %s (protocol version %d)",
                     s_ws_url, s_ws_version);
            result = ESP_OK;
        }
    }
    else
    {
        ESP_LOGW(TAG, "No websocket section in check-in response");
    }

    /* ---- server_time: set the clock so TLS and logs are sane ---- */
    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time))
    {
        cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON *tz_offset = cJSON_GetObjectItem(server_time, "timezone_offset");
        if (cJSON_IsNumber(timestamp))
        {
            double ts = timestamp->valuedouble;
            if (cJSON_IsNumber(tz_offset))
            {
                ts += ((double)tz_offset->valueint * 60.0 * 1000.0);
            }
            struct timeval tv;
            tv.tv_sec = (time_t)(ts / 1000.0);
            tv.tv_usec = (suseconds_t)(((long long)ts % 1000) * 1000);
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "Clock set from server");
        }
    }

    /* ---- firmware: reported only, never auto-flashed ---- */
    cJSON *fw = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(fw))
    {
        cJSON *fv = cJSON_GetObjectItem(fw, "version");
        if (cJSON_IsString(fv))
        {
            const esp_app_desc_t *app_desc = esp_app_get_description();
            if (strcmp(fv->valuestring, app_desc->version) != 0)
            {
                /*
                 * Deliberately not calling esp_https_ota() here: a stray
                 * or hostile check-in response must never be able to
                 * silently reflash a running pet.
                 */
                ESP_LOGI(TAG, "Server has firmware %s (running %s); "
                              "automatic OTA is intentionally disabled",
                         fv->valuestring, app_desc->version);
            }
        }
    }

    cJSON_Delete(root);

    /* An unactivated device gets no websocket target; say so plainly
     * instead of reporting a generic failure. */
    if (result != ESP_OK && s_activation_pending)
    {
        ESP_LOGW(TAG, "Waiting for activation before a channel can open");
    }
    return result;
}

static esp_err_t do_ota_checkin(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();

    /*
     * Body mirrors xiaozhi-esp32's Board::GetSystemInfoJson() closely
     * enough for a server to identify and register the device:
     * version 2, language, flash/heap, MAC, UUID, chip info and
     * application details. The huge partition_table array that xiaozhi
     * also sends is omitted -- servers key off mac_address/uuid, and it
     * would cost several KB of heap on every reconnect.
     */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "version", 2);
    cJSON_AddStringToObject(body, "language", CONFIG_KOYODA_LANGUAGE);
    cJSON_AddNumberToObject(body, "flash_size", (double)s_flash_size);
    cJSON_AddNumberToObject(body, "psram_size", (double)esp_psram_get_size());
    cJSON_AddNumberToObject(body, "minimum_free_heap_size",
                            (double)esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(body, "mac_address", s_mac_str);
    cJSON_AddStringToObject(body, "uuid", s_uuid);
    cJSON_AddStringToObject(body, "chip_model_name", "esp32s3");

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    cJSON *chip = cJSON_CreateObject();
    cJSON_AddNumberToObject(chip, "model", chip_info.model);
    cJSON_AddNumberToObject(chip, "cores", chip_info.cores);
    cJSON_AddNumberToObject(chip, "revision", chip_info.revision);
    cJSON_AddNumberToObject(chip, "features", chip_info.features);
    cJSON_AddItemToObject(body, "chip_info", chip);

    char compile_time[64];
    snprintf(compile_time, sizeof(compile_time), "%sT%sZ",
             app_desc->date, app_desc->time);
    char elf_sha[65];
    for (int i = 0; i < 32; i++)
    {
        snprintf(elf_sha + i * 2, sizeof(elf_sha) - i * 2, "%02x",
                 app_desc->app_elf_sha256[i]);
    }

    cJSON *application = cJSON_CreateObject();
    cJSON_AddStringToObject(application, "name", app_desc->project_name);
    cJSON_AddStringToObject(application, "version", app_desc->version);
    cJSON_AddStringToObject(application, "compile_time", compile_time);
    cJSON_AddStringToObject(application, "idf_version", app_desc->idf_ver);
    cJSON_AddStringToObject(application, "elf_sha256", elf_sha);
    cJSON_AddItemToObject(body, "application", application);

    /* partition_table / ota / display: xiaozhi's official server appears
     * to validate the full body shape, so send all of it rather than the
     * trimmed version. Cost is ~1-2 KB of heap per check-in. */
    cJSON *parts = cJSON_CreateArray();
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL)
    {
        const esp_partition_t *p = esp_partition_get(it);
        cJSON *pj = cJSON_CreateObject();
        cJSON_AddStringToObject(pj, "label", p->label);
        cJSON_AddNumberToObject(pj, "type", p->type);
        cJSON_AddNumberToObject(pj, "subtype", p->subtype);
        cJSON_AddNumberToObject(pj, "address", (double)p->address);
        cJSON_AddNumberToObject(pj, "size", (double)p->size);
        cJSON_AddItemToArray(parts, pj);
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
    cJSON_AddItemToObject(body, "partition_table", parts);

    cJSON *ota_obj = cJSON_CreateObject();
    const esp_partition_t *running = esp_ota_get_running_partition();
    cJSON_AddStringToObject(ota_obj, "label",
                            running != NULL ? running->label : "factory");
    cJSON_AddItemToObject(body, "ota", ota_obj);

    cJSON *display = cJSON_CreateObject();
    cJSON_AddBoolToObject(display, "monochrome", false);
    cJSON_AddNumberToObject(display, "width", 466);
    cJSON_AddNumberToObject(display, "height", 466);
    cJSON_AddItemToObject(body, "display", display);

    cJSON *board_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(board_obj, "type", CONFIG_KOYODA_BOARD_NAME);
    cJSON_AddStringToObject(board_obj, "name", CONFIG_KOYODA_BOARD_NAME);
    cJSON_AddStringToObject(board_obj, "manufacturer", "koyoda");
    {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        {
            cJSON_AddStringToObject(board_obj, "ssid", (const char *)ap.ssid);
            cJSON_AddNumberToObject(board_obj, "rssi", ap.rssi);
            cJSON_AddNumberToObject(board_obj, "channel", ap.primary);
        }
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
        {
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            cJSON_AddStringToObject(board_obj, "ip", ip_str);
        }
    }
    cJSON_AddStringToObject(board_obj, "mac", s_mac_str);
    cJSON_AddItemToObject(body, "board", board_obj);

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (body_str == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t http_cfg = {
        .url = CONFIG_KOYODA_OTA_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL)
    {
        cJSON_free(body_str);
        return ESP_FAIL;
    }

    /* Header set matches xiaozhi-esp32's Ota::SetupHttp(). */
    char user_agent[96];
    snprintf(user_agent, sizeof(user_agent), "%s/%s",
             CONFIG_KOYODA_BOARD_NAME, app_desc->version);

    esp_http_client_set_header(client, "Activation-Version", "1");
    esp_http_client_set_header(client, "Device-Id", s_mac_str);
    esp_http_client_set_header(client, "Client-Id", s_uuid);
    esp_http_client_set_header(client, "User-Agent", user_agent);
    esp_http_client_set_header(client, "Accept-Language", CONFIG_KOYODA_LANGUAGE);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body_str, (int)strlen(body_str));

    esp_err_t err = esp_http_client_perform(client);
    esp_err_t result = ESP_FAIL;

    if (err == ESP_OK)
    {
        int status = esp_http_client_get_status_code(client);
        int64_t content_len = esp_http_client_get_content_length(client);

        if (content_len > 0 && content_len < 8192)
        {
            char *resp = (char *)malloc((size_t)content_len + 1);
            if (resp != NULL)
            {
                int read = esp_http_client_read_response(client, resp, (int)content_len);
                if (read > 0)
                {
                    resp[read] = '\0';
                    if (status == 200)
                    {
                        result = parse_checkin_response(resp);
                    }
                    else
                    {
                        /* Show what the server actually objected to instead
                         * of just the status code. */
                        ESP_LOGW(TAG, "Check-in rejected: HTTP %d", status);
                        ESP_LOGW(TAG, "Server said: %s", resp);
                        ESP_LOGW(TAG, "Request was: %s", body_str);
                    }
                }
                free(resp);
            }
        }
        else
        {
            ESP_LOGW(TAG, "OTA check-in HTTP status=%d len=%lld (no body)",
                     status, (long long)content_len);
        }
    }
    else
    {
        ESP_LOGW(TAG, "OTA check-in failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    cJSON_free(body_str);

    /* TLS is the biggest consumer of DMA-capable internal RAM on this
     * board; report it so a starved LCD is diagnosable from the log. */
    ESP_LOGI(TAG, "heap after check-in: internal %u B (min %u), psram %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    return result;
}

/* ------------------------------------------------------------------------- */
/* WebSocket protocol: hello / listen / tts, matching xiaozhi-esp32's       */
/* WebsocketProtocol JSON shapes (protocols/websocket_protocol.cc).         */
/* ------------------------------------------------------------------------- */

static void send_text(const char *text)
{
    if (s_ws == NULL || !esp_websocket_client_is_connected(s_ws))
    {
        return;
    }
    /* Bounded, never portMAX_DELAY: a stalled server must not wedge the
     * backend worker. Only this task ever sends. */
    esp_websocket_client_send_text(s_ws, text, (int)strlen(text), pdMS_TO_TICKS(500));
}

static void send_hello(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", s_ws_version);
    cJSON *features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "mcp", false);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "format", "opus");
    cJSON_AddNumberToObject(audio, "sample_rate", KOYODA_OPUS_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio, "channels", 1);
    cJSON_AddNumberToObject(audio, "frame_duration", KOYODA_OPUS_FRAME_MS);
    cJSON_AddItemToObject(root, "audio_params", audio);

    char *json = cJSON_PrintUnformatted(root);
    send_text(json);
    cJSON_free(json);
    cJSON_Delete(root);
}

static void send_listen_state(const char *state, const char *mode)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", state);
    if (mode != NULL)
    {
        cJSON_AddStringToObject(root, "mode", mode);
    }
    char *json = cJSON_PrintUnformatted(root);
    send_text(json);
    cJSON_free(json);
    cJSON_Delete(root);
}

static void handle_incoming_json(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, (size_t)len);
    if (root == NULL)
    {
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type))
    {
        if (strcmp(type->valuestring, "hello") == 0)
        {
            xEventGroupSetBits(s_events, WS_HELLO_BIT);
        }
        else if (strcmp(type->valuestring, "tts") == 0)
        {
            cJSON *state = cJSON_GetObjectItem(root, "state");
            if (cJSON_IsString(state))
            {
                if (strcmp(state->valuestring, "start") == 0)
                {
                    s_playback_active = true;
                    koyoda_audio_duplex_playback_start();
                    koyoda_face_state_set(KOYODA_FACE_AI_SPEAKING);
                }
                else if (strcmp(state->valuestring, "stop") == 0)
                {
                    if (s_playback_active)
                    {
                        koyoda_audio_duplex_playback_end();
                        s_playback_active = false;
                    }
                    koyoda_face_state_set(KOYODA_FACE_AI_IDLE);
                }
            }
        }
        else if (strcmp(type->valuestring, "stt") == 0)
        {
            cJSON *text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text))
            {
                ESP_LOGI(TAG, "STT: %s", text->valuestring);
            }
            koyoda_face_state_set(KOYODA_FACE_AI_THINKING);
        }
    }

    cJSON_Delete(root);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        /* Do NOT send from this handler: it runs on the websocket
         * client's own task, and sending with a long timeout from here
         * can deadlock. The worker task sends "hello" instead. */
        xEventGroupSetBits(s_events, WS_CONNECTED_BIT);
        break;

    case WEBSOCKET_EVENT_DATA:
        /*
         * Foundation-level note: this treats every DATA event as one
         * complete frame. esp_websocket_client can deliver a large frame
         * across multiple DATA events (see data->payload_offset /
         * data->payload_len vs data->data_len); a production build should
         * reassemble those before treating data_ptr as a whole message.
         */
        if (data->op_code == 0x1 /* text */)
        {
            handle_incoming_json(data->data_ptr, data->data_len);
        }
        else if (data->op_code == 0x2 /* binary */)
        {
            /* Protocol version 1: the payload is a bare Opus packet. */
            if (s_playback_active && data->data_len > 0 && koyoda_codec_is_open())
            {
                size_t pcm_samples = 0;
                if (koyoda_codec_decode(
                        (const uint8_t *)data->data_ptr,
                        (size_t)data->data_len,
                        s_playback_pcm,
                        KOYODA_CODEC_MAX_PCM_OUT,
                        &pcm_samples) == ESP_OK &&
                    pcm_samples > 0)
                {
                    koyoda_audio_duplex_playback_write(s_playback_pcm, pcm_samples);
                }
            }
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WebSocket disconnected/error");
        s_channel_open = false;
        if (s_playback_active)
        {
            koyoda_audio_duplex_playback_end();
            s_playback_active = false;
        }
        koyoda_face_state_set(KOYODA_FACE_AI_IDLE);
        break;

    default:
        break;
    }
}

static esp_err_t open_ws_channel(void)
{
    if (s_ws_url[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_websocket_client_config_t cfg = {
        .uri = s_ws_url,
        .reconnect_timeout_ms = 0, /* the backend task owns retry/backoff */
        .network_timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == NULL)
    {
        return ESP_FAIL;
    }

    char auth[192];
    if (s_ws_token[0] != '\0')
    {
        if (strchr(s_ws_token, ' ') != NULL)
        {
            snprintf(auth, sizeof(auth), "%s", s_ws_token);
        }
        else
        {
            snprintf(auth, sizeof(auth), "Bearer %s", s_ws_token);
        }
        esp_websocket_client_append_header(s_ws, "Authorization", auth);
    }
    char ver_str[8];
    snprintf(ver_str, sizeof(ver_str), "%d", s_ws_version);
    esp_websocket_client_append_header(s_ws, "Protocol-Version", ver_str);
    esp_websocket_client_append_header(s_ws, "Device-Id", s_mac_str);
    esp_websocket_client_append_header(s_ws, "Client-Id", s_uuid);

    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    xEventGroupClearBits(s_events, WS_HELLO_BIT | WS_CONNECTED_BIT);
    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK)
    {
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return err;
    }

    /* Wait for the TCP/TLS handshake to complete before sending. */
    EventBits_t bits = xEventGroupWaitBits(
        s_events, WS_CONNECTED_BIT, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(BACKEND_WS_HELLO_TIMEOUT_MS));
    if (!(bits & WS_CONNECTED_BIT))
    {
        ESP_LOGW(TAG, "WebSocket did not connect within timeout");
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return ESP_ERR_TIMEOUT;
    }

    send_hello();

    bits = xEventGroupWaitBits(
        s_events, WS_HELLO_BIT, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(BACKEND_WS_HELLO_TIMEOUT_MS));
    if (!(bits & WS_HELLO_BIT))
    {
        ESP_LOGW(TAG, "No server hello within timeout");
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return ESP_ERR_TIMEOUT;
    }

    /* Bring up Opus only now: it costs tens of KB and an idle KOYODA
     * should hold none of it. */
    if (koyoda_codec_open() != ESP_OK)
    {
        ESP_LOGE(TAG, "Codec open failed; closing channel");
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return ESP_FAIL;
    }

    if (s_playback_pcm == NULL)
    {
        s_playback_pcm = heap_caps_malloc(
            KOYODA_CODEC_MAX_PCM_OUT * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (s_playback_pcm == NULL)
        {
            s_playback_pcm = malloc(KOYODA_CODEC_MAX_PCM_OUT * sizeof(int16_t));
        }
        if (s_playback_pcm == NULL)
        {
            ESP_LOGE(TAG, "Playback buffer alloc failed; closing channel");
            koyoda_codec_close();
            esp_websocket_client_stop(s_ws);
            esp_websocket_client_destroy(s_ws);
            s_ws = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    s_channel_open = true;
    koyoda_face_state_set(KOYODA_FACE_AI_IDLE);
    return ESP_OK;
}

static void close_ws_channel(void)
{
    if (s_ws != NULL)
    {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_channel_open = false;
    s_playback_active = false;

    /* Give the codec's tens of KB back so an idle pet holds none of it. */
    koyoda_codec_close();
    free(s_playback_pcm);
    s_playback_pcm = NULL;
}

/* ------------------------------------------------------------------------- */
/* Mic bridge: koyoda_audio_duplex.c calls this from its own audio task on  */
/* every capture block. We only ever enqueue here; the worker task below   */
/* owns the socket and drains the queue.                                    */
/* ------------------------------------------------------------------------- */

static void mic_frame_callback(const int16_t *samples, size_t sample_count, bool vad_speaking, void *ctx)
{
    (void)ctx;

    bool path_open = s_channel_open;
#if CONFIG_KOYODA_ECHO_TEST
    path_open = s_echo_active;
#endif

    if (!koyoda_audio_duplex_ai_is_enabled() || !path_open ||
        s_mic_queue == NULL || s_ctrl_queue == NULL)
    {
        s_prev_vad = false;
        return;
    }

    if (vad_speaking && !s_prev_vad)
    {
        backend_ctrl_t ev = BE_CTRL_LISTEN_START;
        xQueueSend(s_ctrl_queue, &ev, 0);
    }
    else if (!vad_speaking && s_prev_vad)
    {
        backend_ctrl_t ev = BE_CTRL_LISTEN_STOP;
        xQueueSend(s_ctrl_queue, &ev, 0);
    }
    s_prev_vad = vad_speaking;

    if (!vad_speaking || sample_count == 0)
    {
        return;
    }

    mic_frame_msg_t msg;
    msg.sample_count = (uint16_t)(sample_count > BACKEND_MIC_SAMPLES_MAX
                                       ? BACKEND_MIC_SAMPLES_MAX
                                       : sample_count);
    memcpy(msg.samples, samples, msg.sample_count * sizeof(int16_t));

    if (xQueueSend(s_mic_queue, &msg, 0) != pdTRUE)
    {
        /* Queue full: drop this block rather than blocking the mic task. */
    }
}

/* ------------------------------------------------------------------------- */
/* Worker task: owns the state machine, the OTA check-in, the WebSocket     */
/* client, and drains the mic queue.                                        */
/* ------------------------------------------------------------------------- */

/* Emits one encoded Opus frame onto the websocket. Called synchronously
 * from koyoda_codec_encode_push() on the backend worker task. */
static void opus_packet_ready(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (s_ws == NULL || !s_channel_open)
    {
        return;
    }
    esp_websocket_client_send_bin(
        s_ws, (const char *)data, (int)len, pdMS_TO_TICKS(200));
}

#if CONFIG_KOYODA_ECHO_TEST
/* Decode each freshly encoded frame straight back and stash the PCM. */
static void echo_packet_ready(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (s_echo_buf == NULL || s_playback_pcm == NULL)
    {
        return;
    }
    size_t produced = 0;
    if (koyoda_codec_decode(data, len, s_playback_pcm,
                            KOYODA_CODEC_MAX_PCM_OUT, &produced) != ESP_OK)
    {
        return;
    }
    size_t space = ECHO_BUFFER_SAMPLES - s_echo_len;
    if (produced > space)
    {
        produced = space;
    }
    memcpy(&s_echo_buf[s_echo_len], s_playback_pcm, produced * sizeof(int16_t));
    s_echo_len += produced;
}

static void echo_replay(void)
{
    if (s_echo_len == 0)
    {
        return;
    }
    ESP_LOGI(TAG, "ECHO: replaying %u samples (%.2f s)",
             (unsigned)s_echo_len,
             (double)s_echo_len / (double)KOYODA_PCM_SAMPLE_RATE);

    if (koyoda_audio_duplex_playback_start() == ESP_OK)
    {
        const size_t chunk = 512;
        for (size_t off = 0; off < s_echo_len; off += chunk)
        {
            size_t n = s_echo_len - off;
            if (n > chunk) n = chunk;
            koyoda_audio_duplex_playback_write(&s_echo_buf[off], n);
        }
        koyoda_audio_duplex_playback_end();
    }
    s_echo_len = 0;
}

/* Runs instead of the network path when the echo test is compiled in. */
static void echo_tick(bool ai_on)
{
    if (ai_on && !s_echo_active)
    {
        if (koyoda_codec_open() != ESP_OK)
        {
            return;
        }
        if (s_playback_pcm == NULL)
        {
            s_playback_pcm = heap_caps_malloc(
                KOYODA_CODEC_MAX_PCM_OUT * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        }
        if (s_echo_buf == NULL)
        {
            s_echo_buf = heap_caps_malloc(
                ECHO_BUFFER_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        }
        if (s_playback_pcm == NULL || s_echo_buf == NULL)
        {
            ESP_LOGE(TAG, "ECHO: buffer alloc failed");
            koyoda_codec_close();
            return;
        }
        s_echo_len = 0;
        s_echo_active = true;
        ESP_LOGI(TAG, "ECHO TEST ARMED: speak, then pause to hear yourself");
    }
    else if (!ai_on && s_echo_active)
    {
        s_echo_active = false;
        s_echo_len = 0;
        koyoda_codec_close();
        free(s_playback_pcm); s_playback_pcm = NULL;
        free(s_echo_buf);     s_echo_buf = NULL;
        ESP_LOGI(TAG, "ECHO TEST disarmed");
    }

    if (!s_echo_active)
    {
        return;
    }

    backend_ctrl_t ev;
    while (xQueueReceive(s_ctrl_queue, &ev, 0) == pdTRUE)
    {
        if (ev == BE_CTRL_LISTEN_START)
        {
            s_echo_len = 0;
            koyoda_codec_encode_reset();
            ESP_LOGI(TAG, "ECHO: recording...");
        }
        else
        {
            echo_replay();
        }
    }

    mic_frame_msg_t m;
    while (xQueueReceive(s_mic_queue, &m, 0) == pdTRUE)
    {
        koyoda_codec_encode_push(m.samples, m.sample_count, echo_packet_ready, NULL);
    }
}
#endif /* CONFIG_KOYODA_ECHO_TEST */

static void backend_task(void *arg)
{
    (void)arg;
    mic_frame_msg_t msg;

    while (1)
    {
        bool ai_on = koyoda_audio_duplex_ai_is_enabled();

#if CONFIG_KOYODA_ECHO_TEST
        /* Echo test replaces the network path entirely. */
        echo_tick(ai_on);
        vTaskDelay(pdMS_TO_TICKS(ai_on ? BACKEND_ACTIVE_TICK_MS : BACKEND_TICK_MS));
        continue;
#endif

        bool wifi_up = s_wifi_connected;
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        if (!wifi_up || !ai_on)
        {
            if (s_channel_open)
            {
                ESP_LOGI(TAG, "Closing channel (wifi_up=%d ai_on=%d)", wifi_up, ai_on);
                close_ws_channel();
            }
            s_state = wifi_up ? BE_STATE_WIFI_UP : BE_STATE_WIFI_DOWN;
            /* Drain and discard anything queued while AI was toggling. */
            while (xQueueReceive(s_mic_queue, &msg, 0) == pdTRUE) {}
            backend_ctrl_t drop;
            while (xQueueReceive(s_ctrl_queue, &drop, 0) == pdTRUE) {}
            s_prev_vad = false;
        }
        else if (s_reconnect_requested ||
                 (s_state != BE_STATE_WS_OPEN && s_state != BE_STATE_BACKOFF))
        {
            s_reconnect_requested = false;
            ESP_LOGI(TAG, "AI on + Wi-Fi up: starting OTA check-in");
            s_state = BE_STATE_OTA_CHECKIN;

            if (do_ota_checkin() != ESP_OK && s_ws_url[0] == '\0')
            {
                ESP_LOGW(TAG, "OTA check-in gave no websocket target yet; retrying later");
                s_state = BE_STATE_BACKOFF;
                s_backoff_until_ms = now_ms + BACKEND_OTA_RETRY_MS;
            }
            else
            {
                s_state = BE_STATE_WS_CONNECTING;
                if (open_ws_channel() == ESP_OK)
                {
                    s_state = BE_STATE_WS_OPEN;
                    ESP_LOGI(TAG, "Xiaozhi backend channel open");
                }
                else
                {
                    s_state = BE_STATE_BACKOFF;
                    s_backoff_until_ms = now_ms + BACKEND_OTA_RETRY_MS;
                }
            }
        }
        else if (s_state == BE_STATE_BACKOFF && (int32_t)(now_ms - s_backoff_until_ms) >= 0)
        {
            s_state = BE_STATE_WIFI_UP; /* re-enter the branch above next tick */
        }

        /* Send VAD control events raised by the audio task. Doing this
         * here (not in the callback) keeps the audio owner deterministic. */
        backend_ctrl_t ev;
        while (s_channel_open && s_ws != NULL &&
               xQueueReceive(s_ctrl_queue, &ev, 0) == pdTRUE)
        {
            if (ev == BE_CTRL_LISTEN_START)
            {
                send_listen_state("start", "auto");
            }
            else
            {
                send_listen_state("stop", NULL);
                /* Drop any partial frame so the next utterance starts clean. */
                koyoda_codec_encode_reset();
            }
        }

        /* Encode queued mic audio to Opus and send. All of the heavy
         * lifting (resample + encode) happens here on the worker task,
         * never on the audio owner task. */
        while (s_channel_open && s_ws != NULL &&
               xQueueReceive(s_mic_queue, &msg, 0) == pdTRUE)
        {
            koyoda_codec_encode_push(
                msg.samples, msg.sample_count, opus_packet_ready, NULL);
        }

        /* Poll fast while a conversation is live so VAD events and audio
         * are not delayed by a whole idle tick; idle slowly otherwise. */
        vTaskDelay(pdMS_TO_TICKS(s_channel_open ? BACKEND_ACTIVE_TICK_MS
                                                : BACKEND_TICK_MS));
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t koyoda_backend_start(void)
{
#if CONFIG_KOYODA_CODEC_SELFTEST
    /* Runs before anything else claims memory, so a failure here is the
     * codec's fault and not fragmentation. */
    (void)koyoda_codec_selftest_run();
#endif

    load_or_create_identity();

    s_events = xEventGroupCreate();
    if (s_events == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    /*
     * Put the mic buffer in PSRAM. Created with plain xQueueCreate it
     * lands in internal RAM, which on this board is the same pool the
     * LCD needs for DMA-capable flush buffers -- that starvation shows
     * up as torn lines and a stuttering face on page changes.
     * Only the control block stays internal; it is ~80 bytes and this
     * queue is never touched from an ISR.
     */
    s_mic_queue_storage = heap_caps_malloc(
        BACKEND_MIC_QUEUE_DEPTH * sizeof(mic_frame_msg_t),
        MALLOC_CAP_SPIRAM);

    if (s_mic_queue_storage != NULL)
    {
        s_mic_queue = xQueueCreateStatic(
            BACKEND_MIC_QUEUE_DEPTH,
            sizeof(mic_frame_msg_t),
            s_mic_queue_storage,
            &s_mic_queue_struct);
    }
    else
    {
        /* No PSRAM available: fall back rather than refusing to start. */
        ESP_LOGW(TAG, "PSRAM unavailable; mic queue falls back to internal RAM");
        s_mic_queue = xQueueCreate(BACKEND_MIC_QUEUE_DEPTH, sizeof(mic_frame_msg_t));
    }

    if (s_mic_queue == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_ctrl_queue = xQueueCreate(8, sizeof(backend_ctrl_t));
    if (s_ctrl_queue == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    koyoda_audio_duplex_set_frame_callback(mic_frame_callback, NULL);

    BaseType_t ok = xTaskCreatePinnedToCore(
        backend_task,
        "koyoda_backend",
        BACKEND_TASK_STACK_BYTES,
        NULL,
        BACKEND_TASK_PRIORITY,
        &s_task,
        BACKEND_TASK_CORE);
    if (ok != pdPASS)
    {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Xiaozhi backend foundation ready; OTA URL=%s", CONFIG_KOYODA_OTA_URL);
    return ESP_OK;
}

void koyoda_backend_notify_wifi_state(bool connected)
{
    s_wifi_connected = connected;
    if (connected)
    {
        s_reconnect_requested = true;
    }
}

bool koyoda_backend_is_channel_open(void)
{
    return s_channel_open;
}

void koyoda_backend_request_reconnect(void)
{
    s_reconnect_requested = true;
}
