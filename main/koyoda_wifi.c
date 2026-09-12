#include "koyoda_wifi.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "koyoda_backend.h"

#if __has_include("koyoda_wifi_secrets.h")
#include "koyoda_wifi_secrets.h"
#else
#define KOYODA_WIFI_SSID ""
#define KOYODA_WIFI_PASSWORD ""
#endif

static const char *TAG = "KOYODA_WIFI";

#define WIFI_START_DELAY_MS             1500
#define WIFI_SETUP_TIMEOUT_MS           (5U * 60U * 1000U)
#define WIFI_SETUP_SUCCESS_HOLD_MS      3000U
#define WIFI_TEST_TIMEOUT_MS            15000U
#define WIFI_SCAN_MAX_AP                12
#define WIFI_SCAN_FIRST_CHANNEL         1
#define WIFI_SCAN_LAST_CHANNEL          13
#define WIFI_SCAN_ACTIVE_MIN_MS         25
#define WIFI_SCAN_ACTIVE_MAX_MS         50

#define WIFI_TEST_GOT_IP_BIT            BIT0
#define WIFI_TEST_DISCONNECTED_BIT      BIT1

#define WIFI_NVS_NAMESPACE              "koyoda_wifi"
#define WIFI_NVS_SSID_KEY               "ssid"
#define WIFI_NVS_PASS_KEY               "pass"

static volatile bool s_started = false;
static volatile bool s_wifi_ready = false;
static volatile bool s_provisioning = false;
static volatile bool s_provision_stop_requested = false;
static volatile bool s_testing_credentials = false;
static volatile bool s_connected = false;
static volatile int s_rssi = -127;
static volatile koyoda_wifi_setup_state_t s_setup_state = KOYODA_WIFI_SETUP_OFF;

static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static char s_current_ssid[33] = {0};
static char s_active_ssid[33] = {0};
static char s_active_password[65] = {0};
static char s_pending_ssid[33] = {0};
static char s_pending_password[65] = {0};

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static esp_event_handler_instance_t s_wifi_handler = NULL;
static esp_event_handler_instance_t s_ip_handler = NULL;
static EventGroupHandle_t s_test_events = NULL;

static httpd_handle_t s_http_server = NULL;
static TaskHandle_t s_provision_task = NULL;
static TaskHandle_t s_apply_task = NULL;

static wifi_ap_record_t s_scan_records[WIFI_SCAN_MAX_AP];
static volatile uint16_t s_scan_count = 0;
static volatile bool s_scan_requested = false;
static volatile bool s_scan_in_progress = false;
static volatile bool s_scan_done_pending = false;
static volatile bool s_scan_ready = false;
static volatile uint8_t s_scan_channel = WIFI_SCAN_FIRST_CHANNEL;

/* ------------------------------------------------------------------------- */
/* Small shared-state helpers.                                               */
/* ------------------------------------------------------------------------- */

static void set_connected_state(bool connected, int rssi, const char *ssid)
{
    portENTER_CRITICAL(&s_state_mux);
    s_connected = connected;
    s_rssi = connected ? rssi : -127;
    if (connected && ssid != NULL)
    {
        strlcpy(s_current_ssid, ssid, sizeof(s_current_ssid));
    }
    else if (!connected)
    {
        s_current_ssid[0] = '\0';
    }
    portEXIT_CRITICAL(&s_state_mux);
}

static void set_setup_state(koyoda_wifi_setup_state_t state)
{
    portENTER_CRITICAL(&s_state_mux);
    s_setup_state = state;
    portEXIT_CRITICAL(&s_state_mux);
}

bool koyoda_wifi_is_connected(void)
{
    bool value;
    portENTER_CRITICAL(&s_state_mux);
    value = s_connected;
    portEXIT_CRITICAL(&s_state_mux);
    return value;
}

int koyoda_wifi_get_rssi(void)
{
    int value;
    portENTER_CRITICAL(&s_state_mux);
    value = s_rssi;
    portEXIT_CRITICAL(&s_state_mux);
    return value;
}

void koyoda_wifi_get_ssid(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0)
    {
        return;
    }

    portENTER_CRITICAL(&s_state_mux);
    strlcpy(out, s_current_ssid, out_size);
    portEXIT_CRITICAL(&s_state_mux);
}

bool koyoda_wifi_is_provisioning(void)
{
    return s_provisioning;
}

koyoda_wifi_setup_state_t koyoda_wifi_get_setup_state(void)
{
    koyoda_wifi_setup_state_t state;
    portENTER_CRITICAL(&s_state_mux);
    state = s_setup_state;
    portEXIT_CRITICAL(&s_state_mux);
    return state;
}

/* ------------------------------------------------------------------------- */
/* NVS: one last-known-good network.                                         */
/* ------------------------------------------------------------------------- */

static esp_err_t init_nvs_safe(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        /* This is the standard ESP-IDF recovery path for an unusable NVS
         * partition. It is intentionally only used for these two errors. */
        ret = nvs_flash_erase();
        if (ret != ESP_OK)
        {
            return ret;
        }
        ret = nvs_flash_init();
    }

    return ret;
}

static bool load_saved_credentials(char *ssid, size_t ssid_size,
                                   char *password, size_t password_size)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    {
        return false;
    }

    size_t ssid_len = ssid_size;
    size_t pass_len = password_size;

    esp_err_t a = nvs_get_str(handle, WIFI_NVS_SSID_KEY, ssid, &ssid_len);
    esp_err_t b = nvs_get_str(handle, WIFI_NVS_PASS_KEY, password, &pass_len);
    nvs_close(handle);

    if (a != ESP_OK || b != ESP_OK || ssid[0] == '\0')
    {
        ssid[0] = '\0';
        password[0] = '\0';
        return false;
    }

    return true;
}

static esp_err_t save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = nvs_set_str(handle, WIFI_NVS_SSID_KEY, ssid);
    if (ret == ESP_OK)
    {
        ret = nvs_set_str(handle, WIFI_NVS_PASS_KEY, password);
    }
    if (ret == ESP_OK)
    {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

/* ------------------------------------------------------------------------- */
/* STA configuration and normal reconnect behavior.                          */
/* ------------------------------------------------------------------------- */

static esp_err_t apply_sta_config(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t ssid_len = strnlen(ssid, 33);
    const size_t pass_len = strnlen(password ? password : "", 65);

    if (ssid_len == 0 || ssid_len > 32 || pass_len > 64)
    {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, ssid, ssid_len);
    if (pass_len > 0)
    {
        memcpy(cfg.sta.password, password, pass_len);
    }

    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

static void remember_active_credentials(const char *ssid, const char *password)
{
    portENTER_CRITICAL(&s_state_mux);
    strlcpy(s_active_ssid, ssid ? ssid : "", sizeof(s_active_ssid));
    strlcpy(s_active_password, password ? password : "", sizeof(s_active_password));
    portEXIT_CRITICAL(&s_state_mux);
}

static bool copy_active_credentials(char *ssid, size_t ssid_size,
                                    char *password, size_t password_size)
{
    bool have;
    portENTER_CRITICAL(&s_state_mux);
    strlcpy(ssid, s_active_ssid, ssid_size);
    strlcpy(password, s_active_password, password_size);
    have = (s_active_ssid[0] != '\0');
    portEXIT_CRITICAL(&s_state_mux);
    return have;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE)
    {
        /* Do no allocation/sorting in the system event-loop callback. The
         * low-priority provisioning task will collect the completed results. */
        s_scan_in_progress = false;
        if (!s_testing_credentials)
        {
            s_scan_done_pending = true;
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        /* Entering APSTA for phone setup must not kick off a fresh STA
         * association.  A connect-in-progress prevents esp_wifi_scan_start().
         * Keep any already-established STA link, but do not start a new one. */
        if (s_provisioning && !s_testing_credentials)
        {
            ESP_LOGI(TAG, "Provisioning active; STA start will not auto-connect");
            return;
        }

        char ssid[33];
        char password[65];
        if (copy_active_credentials(ssid, sizeof(ssid), password, sizeof(password)))
        {
            ESP_LOGI(TAG, "Station started; connecting...");
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        set_connected_state(false, -127, NULL);

        if (!s_testing_credentials)
        {
            koyoda_backend_notify_wifi_state(false);
        }

        if (s_testing_credentials)
        {
            const wifi_event_sta_disconnected_t *disc =
                (const wifi_event_sta_disconnected_t *)event_data;

            /* Our own disconnect is only the hand-off from the old network
             * to the candidate. Never count it as a failed password test. */
            if (disc != NULL && disc->reason == WIFI_REASON_ASSOC_LEAVE)
            {
                return;
            }

            if (s_test_events != NULL)
            {
                xEventGroupSetBits(s_test_events, WIFI_TEST_DISCONNECTED_BIT);
            }
            return;
        }

        /* During phone setup, do not immediately reconnect the old STA.
         * If the old AP is absent, that reconnect loop keeps the radio in
         * CONNECTING and every portal scan is rejected with
         * ESP_ERR_WIFI_STATE.  We do NOT force a disconnect here; an existing
         * good STA connection is allowed to remain up. */
        if (s_provisioning)
        {
            ESP_LOGI(TAG, "Provisioning active; STA reconnect paused");
            return;
        }

        char ssid[33];
        char password[65];
        if (copy_active_credentials(ssid, sizeof(ssid), password, sizeof(password)))
        {
            ESP_LOGW(TAG, "Disconnected; reconnecting...");
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        wifi_ap_record_t ap = {0};
        int rssi = -127;
        char ssid[33] = {0};

        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        {
            rssi = ap.rssi;
            size_t n = strnlen((const char *)ap.ssid, sizeof(ap.ssid));
            if (n > 32) n = 32;
            memcpy(ssid, ap.ssid, n);
            ssid[n] = '\0';
        }

        set_connected_state(true, rssi, ssid);

        ESP_LOGI(TAG, "Connected; IP=" IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "SSID=%s RSSI=%d dBm", ssid[0] ? ssid : "(unknown)", rssi);

        /* Xiaozhi backend foundation: only a state notification, never
         * touches LVGL and never blocks this event handler. */
        if (!s_testing_credentials)
        {
            koyoda_backend_notify_wifi_state(true);
        }

        if (s_testing_credentials && s_test_events != NULL)
        {
            xEventGroupSetBits(s_test_events, WIFI_TEST_GOT_IP_BIT);
            return;
        }

    }
}

/* ------------------------------------------------------------------------- */
/* Wi-Fi scanning used by the phone portal.                                  */
/* ------------------------------------------------------------------------- */

static int compare_ap_rssi(const void *a, const void *b)
{
    const wifi_ap_record_t *aa = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *bb = (const wifi_ap_record_t *)b;
    return (int)bb->rssi - (int)aa->rssi;
}

static void finish_incremental_scan(void)
{
    if (s_scan_count > 1)
    {
        qsort(s_scan_records, s_scan_count, sizeof(s_scan_records[0]), compare_ap_rssi);
    }

    s_scan_requested = false;
    s_scan_in_progress = false;
    s_scan_done_pending = false;
    s_scan_ready = true;
    ESP_LOGI(TAG, "Incremental portal scan complete: %u network(s)",
             (unsigned)s_scan_count);
}

static esp_err_t start_network_scan_async(void)
{
    /* SAFE v2.3: scan ONE channel per request, then yield back to LVGL/Wi-Fi.
     * A long all-channel scan was visibly starving this board's display path.
     * Active probe scans are short, while the 250 ms provisioning loop gives
     * the radio/UI breathing room between channels. */
    if (s_scan_channel > WIFI_SCAN_LAST_CHANNEL)
    {
        finish_incremental_scan();
        return ESP_OK;
    }

    wifi_scan_config_t scan = {0};
    scan.show_hidden = false;
    scan.channel = s_scan_channel;
    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan.scan_time.active.min = WIFI_SCAN_ACTIVE_MIN_MS;
    scan.scan_time.active.max = WIFI_SCAN_ACTIVE_MAX_MS;
    scan.home_chan_dwell_time = 30;

    s_scan_requested = false;
    s_scan_done_pending = false;

    esp_err_t ret = esp_wifi_scan_start(&scan, false);
    if (ret == ESP_OK)
    {
        s_scan_in_progress = true;
        ESP_LOGI(TAG, "Incremental portal scan: channel %u/%u",
                 (unsigned)s_scan_channel, (unsigned)WIFI_SCAN_LAST_CHANNEL);
    }
    else if (ret == ESP_ERR_WIFI_STATE)
    {
        /* Never disconnect just to make scanning work. A previous association
         * may still be finishing. Leave the UI/AP alone and retry later. */
        s_scan_in_progress = false;
        s_scan_requested = true;
        ESP_LOGI(TAG, "STA still connecting; defer scan channel %u",
                 (unsigned)s_scan_channel);
    }
    else
    {
        /* Do not wedge provisioning on one bad channel/error. Skip it and
         * continue the incremental scan on the next provisioning-loop tick. */
        s_scan_in_progress = false;
        ESP_LOGW(TAG, "Scan channel %u failed: %s",
                 (unsigned)s_scan_channel, esp_err_to_name(ret));
        s_scan_channel++;
        if (s_scan_channel > WIFI_SCAN_LAST_CHANNEL)
        {
            finish_incremental_scan();
        }
        else
        {
            s_scan_requested = true;
        }
    }
    return ret;
}

static void collect_scan_results(void)
{
    s_scan_done_pending = false;

    uint16_t count = WIFI_SCAN_MAX_AP;
    wifi_ap_record_t records[WIFI_SCAN_MAX_AP];
    memset(records, 0, sizeof(records));

    esp_err_t ret = esp_wifi_scan_get_ap_records(&count, records);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not read scan channel %u: %s",
                 (unsigned)s_scan_channel, esp_err_to_name(ret));
        /* Clear driver-owned scan memory if record retrieval failed. */
        esp_wifi_clear_ap_list();
    }
    else
    {
        for (uint16_t i = 0; i < count; ++i)
        {
            if (records[i].ssid[0] == '\0')
            {
                continue;
            }

            int existing = -1;
            for (uint16_t j = 0; j < s_scan_count; ++j)
            {
                if (strncmp((const char *)s_scan_records[j].ssid,
                            (const char *)records[i].ssid,
                            sizeof(records[i].ssid)) == 0)
                {
                    existing = (int)j;
                    break;
                }
            }

            if (existing >= 0)
            {
                if (records[i].rssi > s_scan_records[existing].rssi)
                {
                    s_scan_records[existing] = records[i];
                }
            }
            else if (s_scan_count < WIFI_SCAN_MAX_AP)
            {
                s_scan_records[s_scan_count++] = records[i];
            }
        }
    }

    s_scan_in_progress = false;
    s_scan_channel++;

    if (s_scan_channel > WIFI_SCAN_LAST_CHANNEL)
    {
        finish_incremental_scan();
    }
    else
    {
        /* Let the provisioning loop yield ~250 ms before the next channel. */
        s_scan_requested = true;
    }
}

/* ------------------------------------------------------------------------- */
/* Captive portal HTML helpers.                                              */
/* ------------------------------------------------------------------------- */

static void html_escape(const char *src, char *dst, size_t dst_size)
{
    if (dst_size == 0) return;

    size_t used = 0;
    dst[0] = '\0';

    for (const unsigned char *p = (const unsigned char *)src; *p; ++p)
    {
        const char *rep = NULL;
        switch (*p)
        {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default: break;
        }

        if (rep != NULL)
        {
            size_t len = strlen(rep);
            if (used + len >= dst_size) break;
            memcpy(dst + used, rep, len);
            used += len;
        }
        else
        {
            if (used + 1 >= dst_size) break;
            dst[used++] = (char)*p;
        }
    }

    dst[used] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *src, char *dst, size_t dst_size)
{
    if (dst_size == 0) return;

    size_t used = 0;
    while (*src && used + 1 < dst_size)
    {
        if (*src == '+')
        {
            dst[used++] = ' ';
            ++src;
        }
        else if (*src == '%' && src[1] && src[2])
        {
            int hi = hex_value(src[1]);
            int lo = hex_value(src[2]);
            if (hi >= 0 && lo >= 0)
            {
                dst[used++] = (char)((hi << 4) | lo);
                src += 3;
            }
            else
            {
                dst[used++] = *src++;
            }
        }
        else
        {
            dst[used++] = *src++;
        }
    }

    dst[used] = '\0';
}

static bool form_value(const char *body, const char *key,
                       char *out, size_t out_size)
{
    const size_t key_len = strlen(key);
    const char *p = body;

    while (p && *p)
    {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));

        if (eq != NULL && (size_t)(eq - p) == key_len &&
            strncmp(p, key, key_len) == 0)
        {
            size_t raw_len = (size_t)(end - eq - 1);
            char raw[192];
            if (raw_len >= sizeof(raw)) raw_len = sizeof(raw) - 1;
            memcpy(raw, eq + 1, raw_len);
            raw[raw_len] = '\0';
            url_decode(raw, out, out_size);
            return true;
        }

        p = amp ? amp + 1 : NULL;
    }

    if (out_size > 0) out[0] = '\0';
    return false;
}

static const char *setup_state_message(void)
{
    switch (koyoda_wifi_get_setup_state())
    {
        case KOYODA_WIFI_SETUP_STARTING: return "Starting setup hotspot...";
        case KOYODA_WIFI_SETUP_READY: return "Setup hotspot is ready. Type your 2.4 GHz Wi-Fi below.";
        case KOYODA_WIFI_SETUP_TESTING: return "Testing the new Wi-Fi. Please wait...";
        case KOYODA_WIFI_SETUP_FAILED: return "Connection failed. Your old Wi-Fi was kept; check the password and try again.";
        case KOYODA_WIFI_SETUP_SUCCESS: return "Connected and saved. KOYODA is closing setup mode.";
        default: return "Wi-Fi setup is ready.";
    }
}

static esp_err_t portal_root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");

    static const char *head =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>KOYODA Wi-Fi</title><style>"
        "body{margin:0;background:#071015;color:#eefcff;font-family:system-ui,-apple-system,sans-serif}"
        ".wrap{max-width:520px;margin:auto;padding:28px 18px 48px}.card{background:#0e1b22;border:1px solid #21404b;border-radius:22px;padding:22px;box-shadow:0 16px 40px #0007}"
        "h1{margin:0 0 6px;color:#67f3ef;font-size:28px}.sub{color:#9db4bb;margin:0 0 20px}.status{background:#102a31;border-radius:14px;padding:13px 14px;margin:14px 0;color:#dff}"
        "label{display:block;margin:15px 0 6px;font-weight:650}select,input{width:100%;box-sizing:border-box;padding:14px;border-radius:12px;border:1px solid #31515a;background:#071015;color:white;font-size:16px}"
        "button{width:100%;padding:15px;margin-top:20px;border:0;border-radius:14px;background:#43d9d4;color:#042025;font-size:17px;font-weight:800}"
        ".small{font-size:13px;color:#8ea6ad;line-height:1.45}.links{text-align:center;margin-top:16px}.links a{color:#67f3ef;text-decoration:none}.rssi{color:#91abb2;font-size:12px}"
        "</style></head><body><div class='wrap'><div class='card'>"
        "<h1>KOYODA Wi-Fi</h1><p class='sub'>Connect KOYODA to a new network</p><div class='status'>";

    httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, setup_state_message(), HTTPD_RESP_USE_STRLEN);
    /* The first GET merely requests a scan. The provisioning task owns the
     * actual esp_wifi_scan_start() call, so the HTTP task stays lightweight. */
    if (!s_scan_ready && !s_scan_in_progress && !s_scan_requested &&
        !s_testing_credentials)
    {
        if (s_scan_channel < WIFI_SCAN_FIRST_CHANNEL ||
            s_scan_channel > WIFI_SCAN_LAST_CHANNEL)
        {
            s_scan_channel = WIFI_SCAN_FIRST_CHANNEL;
        }
        s_scan_requested = true;
    }

    httpd_resp_send_chunk(req,
        "</div><form method='post' action='/save'>"
        "<label>Nearby Wi-Fi</label><select name='ssid'>",
        HTTPD_RESP_USE_STRLEN);

    const bool scan_ready = s_scan_ready;
    const uint16_t scan_count = s_scan_count;

    if (!scan_ready)
    {
        char scanning_option[128];
        snprintf(scanning_option, sizeof(scanning_option),
                 "<option value=''>Scanning nearby networks... channel %u/%u</option>",
                 (unsigned)s_scan_channel, (unsigned)WIFI_SCAN_LAST_CHANNEL);
        httpd_resp_send_chunk(req, scanning_option, HTTPD_RESP_USE_STRLEN);
    }
    else if (scan_count == 0)
    {
        httpd_resp_send_chunk(req,
            "<option value=''>No network found - type SSID below</option>",
            HTTPD_RESP_USE_STRLEN);
    }
    else
    {
        for (uint16_t i = 0; i < scan_count && i < WIFI_SCAN_MAX_AP; ++i)
        {
            char ssid[33] = {0};
            size_t n = strnlen((const char *)s_scan_records[i].ssid,
                               sizeof(s_scan_records[i].ssid));
            if (n > 32) n = 32;
            memcpy(ssid, s_scan_records[i].ssid, n);

            char escaped[192];
            html_escape(ssid, escaped, sizeof(escaped));

            const char *security =
                (s_scan_records[i].authmode == WIFI_AUTH_OPEN) ? "open" : "locked";
            char option[320];
            snprintf(option, sizeof(option),
                     "<option value=\"%s\">%s  ·  %d dBm  ·  %s</option>",
                     escaped, escaped, (int)s_scan_records[i].rssi, security);
            httpd_resp_send_chunk(req, option, HTTPD_RESP_USE_STRLEN);
        }
    }

    static const char *tail =
        "</select>"
        "<label>Other / hidden Wi-Fi <span class='rssi'>(optional)</span></label>"
        "<input name='ssid_manual' maxlength='32' autocomplete='off' placeholder='Type SSID only if it is not listed'>"
        "<label>Password</label><input name='password' type='password' maxlength='64' autocomplete='new-password' placeholder='Leave blank for an open network'>"
        "<button type='submit'>CONNECT KOYODA</button></form>"
        "<p class='small'>The hotspot starts first. KOYODA scans one 2.4 GHz channel at a time after this page is opened so the display stays responsive. A new network is saved only after KOYODA obtains an IP address.</p>"
        "</div></div>";

    httpd_resp_send_chunk(req, tail, HTTPD_RESP_USE_STRLEN);
    /* SAFE v2.3.2 diagnostic: deliberately do not auto-refresh the portal.
     * The phone/browser can be refreshed manually after the incremental scan
     * finishes.  This keeps the first post-scan HTTP render completely out of
     * the scan-completion path and changes no Wi-Fi/task/boot configuration. */
    httpd_resp_send_chunk(req, "</body></html>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void apply_new_credentials_task(void *arg)
{
    (void)arg;

    /* Let the HTTP POST handler finish sending its acknowledgement before
     * the STA changes channel/network. In APSTA mode a channel change can
     * briefly interrupt the phone's connection to KOYODA-Setup. */
    vTaskDelay(pdMS_TO_TICKS(600));

    char ssid[33];
    char password[65];
    portENTER_CRITICAL(&s_state_mux);
    strlcpy(ssid, s_pending_ssid, sizeof(ssid));
    strlcpy(password, s_pending_password, sizeof(password));
    portEXIT_CRITICAL(&s_state_mux);

    set_setup_state(KOYODA_WIFI_SETUP_TESTING);
    s_testing_credentials = true;

    /* Do not let a background scan overlap the credential association test. */
    if (s_scan_in_progress)
    {
        esp_wifi_scan_stop();
        s_scan_in_progress = false;
        s_scan_done_pending = false;
    }

    xEventGroupClearBits(s_test_events,
                         WIFI_TEST_GOT_IP_BIT | WIFI_TEST_DISCONNECTED_BIT);

    /* This intentional disconnect is ignored by reason in the event handler. */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(250));
    xEventGroupClearBits(s_test_events,
                         WIFI_TEST_GOT_IP_BIT | WIFI_TEST_DISCONNECTED_BIT);

    esp_err_t ret = apply_sta_config(ssid, password);
    if (ret == ESP_OK)
    {
        ret = esp_wifi_connect();
    }

    EventBits_t bits = 0;
    if (ret == ESP_OK)
    {
        bits = xEventGroupWaitBits(
            s_test_events,
            WIFI_TEST_GOT_IP_BIT | WIFI_TEST_DISCONNECTED_BIT,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(WIFI_TEST_TIMEOUT_MS));
    }

    bool success = (ret == ESP_OK) && ((bits & WIFI_TEST_GOT_IP_BIT) != 0);

    if (success)
    {
        ret = save_credentials(ssid, password);
        if (ret == ESP_OK)
        {
            remember_active_credentials(ssid, password);
            s_testing_credentials = false;
            set_setup_state(KOYODA_WIFI_SETUP_SUCCESS);
            ESP_LOGI(TAG, "New Wi-Fi verified and saved: %s", ssid);
            vTaskDelay(pdMS_TO_TICKS(WIFI_SETUP_SUCCESS_HOLD_MS));
            s_provision_stop_requested = true;
        }
        else
        {
            ESP_LOGE(TAG, "New Wi-Fi connected but NVS save failed: %s",
                     esp_err_to_name(ret));
            set_setup_state(KOYODA_WIFI_SETUP_FAILED);
        }
    }
    else
    {
        ESP_LOGW(TAG, "New Wi-Fi test failed; keeping previous saved network");
        set_setup_state(KOYODA_WIFI_SETUP_FAILED);
        s_testing_credentials = false;

        char old_ssid[33];
        char old_password[65];
        if (copy_active_credentials(old_ssid, sizeof(old_ssid),
                                    old_password, sizeof(old_password)))
        {
            apply_sta_config(old_ssid, old_password);
            esp_wifi_connect();
        }
    }

    s_testing_credentials = false;
    s_apply_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t portal_save_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 512)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form data");
        return ESP_FAIL;
    }

    char body[513];
    int total = 0;
    while (total < req->content_len)
    {
        int got = httpd_req_recv(req, body + total, req->content_len - total);
        if (got <= 0)
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not read request");
            return ESP_FAIL;
        }
        total += got;
    }
    body[total] = '\0';

    char selected[33] = {0};
    char manual[33] = {0};
    char password[65] = {0};
    form_value(body, "ssid", selected, sizeof(selected));
    form_value(body, "ssid_manual", manual, sizeof(manual));
    form_value(body, "password", password, sizeof(password));

    const char *ssid = manual[0] ? manual : selected;
    if (ssid[0] == '\0' || strnlen(ssid, 33) > 32 || strnlen(password, 65) > 64)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SSID or password length");
        return ESP_FAIL;
    }

    if (s_apply_task != NULL || s_testing_credentials)
    {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, "KOYODA is already testing a network", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_state_mux);
    strlcpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
    strlcpy(s_pending_password, password, sizeof(s_pending_password));
    portEXIT_CRITICAL(&s_state_mux);

    BaseType_t created = xTaskCreate(
        apply_new_credentials_task,
        "wifi_apply",
        4096,
        NULL,
        1,
        &s_apply_task);

    if (created != pdPASS)
    {
        s_apply_task = NULL;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not start Wi-Fi test");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req,
        "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='3;url=/'>"
        "<body style='background:#071015;color:white;font-family:system-ui;padding:30px'>"
        "<h2 style='color:#67f3ef'>Testing Wi-Fi...</h2>"
        "<p>KOYODA is trying the new network. This page will refresh automatically.</p></body>",
        HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t portal_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Open KOYODA Wi-Fi setup", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL)
    {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_http_server, &config);
    if (ret != ESP_OK)
    {
        s_http_server = NULL;
        return ret;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = portal_root_get,
        .user_ctx = NULL,
    };
    const httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = portal_save_post,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_http_server, &root);
    httpd_register_uri_handler(s_http_server, &save);
    httpd_register_err_handler(s_http_server, HTTPD_404_NOT_FOUND, portal_404);

    return ESP_OK;
}

static void stop_http_server(void)
{
    if (s_http_server != NULL)
    {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
}

/* ------------------------------------------------------------------------- */
/* Provisioning lifecycle.                                                   */
/* ------------------------------------------------------------------------- */

static esp_err_t enable_setup_ap(void)
{
    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid,
            KOYODA_WIFI_SETUP_AP_SSID,
            sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(KOYODA_WIFI_SETUP_AP_SSID);
    strlcpy((char *)ap_cfg.ap.password,
            KOYODA_WIFI_SETUP_AP_PASSWORD,
            sizeof(ap_cfg.ap.password));
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 2;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.ssid_hidden = 0;
    ap_cfg.ap.beacon_interval = 100;
    ap_cfg.ap.pmf_cfg.required = false;

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret == ESP_OK)
    {
        ret = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    }

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "SoftAP configured: ssid=%s hidden=0 channel=%u",
                 KOYODA_WIFI_SETUP_AP_SSID, (unsigned)ap_cfg.ap.channel);
    }
    return ret;
}

static void provisioning_task(void *arg)
{
    (void)arg;

    set_setup_state(KOYODA_WIFI_SETUP_STARTING);
    s_provision_stop_requested = false;

    /* SAFE v2: AP-FIRST. Never perform a Wi-Fi scan on the setup-start path.
     * On this board the Wi-Fi driver and LVGL/display share CPU0, and a
     * blocking scan can starve the UI before the phone hotspot exists. */
    s_scan_count = 0;
    memset(s_scan_records, 0, sizeof(s_scan_records));
    s_scan_channel = WIFI_SCAN_FIRST_CHANNEL;
    s_scan_requested = false;
    s_scan_in_progress = false;
    s_scan_done_pending = false;
    s_scan_ready = false;
    ESP_LOGI(TAG, "SAFE v2.1 stage 1/2: enabling KOYODA-Setup immediately");
    esp_err_t ret = enable_setup_ap();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not start setup AP: %s", esp_err_to_name(ret));
        s_provisioning = false;
        set_setup_state(KOYODA_WIFI_SETUP_OFF);
        s_provision_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* Give the Wi-Fi driver one scheduler slice to start AP beacons before
     * starting the HTTP server. This is asynchronous and does not touch LVGL. */
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_netif_ip_info_t ap_ip = {0};
    if (s_ap_netif != NULL && esp_netif_get_ip_info(s_ap_netif, &ap_ip) == ESP_OK)
    {
        ESP_LOGI(TAG, "KOYODA-Setup AP IP=" IPSTR, IP2STR(&ap_ip.ip));
    }

    ESP_LOGI(TAG, "SAFE v2.1 stage 2/2: starting HTTP portal");
    ret = start_http_server();

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Provisioning service start failed: %s", esp_err_to_name(ret));
        stop_http_server();
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_provisioning = false;
        set_setup_state(KOYODA_WIFI_SETUP_OFF);
        s_provision_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    set_setup_state(KOYODA_WIFI_SETUP_READY);
    ESP_LOGI(TAG, "SAFE v2.1 AP-FIRST ready: SSID=%s password=%s URL=http://192.168.4.1",
             KOYODA_WIFI_SETUP_AP_SSID,
             KOYODA_WIFI_SETUP_AP_PASSWORD);

    TickType_t started_at = xTaskGetTickCount();

    while (!s_provision_stop_requested)
    {
        if (!s_testing_credentials && s_scan_requested &&
            !s_scan_in_progress && !s_scan_ready)
        {
            start_network_scan_async();
        }

        if (!s_testing_credentials && s_scan_done_pending)
        {
            collect_scan_results();
        }

        if (!s_testing_credentials)
        {
            uint32_t elapsed_ms =
                (uint32_t)((xTaskGetTickCount() - started_at) * portTICK_PERIOD_MS);
            if (elapsed_ms >= WIFI_SETUP_TIMEOUT_MS)
            {
                ESP_LOGI(TAG, "Wi-Fi setup timed out");
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }

    stop_http_server();
    if (s_scan_in_progress)
    {
        esp_wifi_scan_stop();
        s_scan_in_progress = false;
    }
    s_scan_requested = false;
    s_scan_done_pending = false;
    esp_wifi_set_mode(WIFI_MODE_STA);

    s_provisioning = false;
    s_provision_stop_requested = false;
    set_setup_state(KOYODA_WIFI_SETUP_OFF);
    s_provision_task = NULL;

    /* If setup ended while not connected, return to the last-known-good Wi-Fi. */
    if (!koyoda_wifi_is_connected() && !s_testing_credentials)
    {
        char ssid[33];
        char password[65];
        if (copy_active_credentials(ssid, sizeof(ssid), password, sizeof(password)))
        {
            apply_sta_config(ssid, password);
            esp_wifi_connect();
        }
    }

    ESP_LOGI(TAG, "Phone setup closed");
    vTaskDelete(NULL);
}

esp_err_t koyoda_wifi_begin_provisioning(void)
{
    if (!s_wifi_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_provisioning || s_provision_task != NULL)
    {
        return ESP_OK;
    }

    s_provisioning = true;
    set_setup_state(KOYODA_WIFI_SETUP_STARTING);

    BaseType_t result = xTaskCreate(
        provisioning_task,
        "wifi_setup",
        4096,
        NULL,
        1,
        &s_provision_task);

    if (result != pdPASS)
    {
        s_provisioning = false;
        s_provision_task = NULL;
        set_setup_state(KOYODA_WIFI_SETUP_OFF);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Initial startup. NVS wins; GitHub-secret credentials are fallback only.   */
/* ------------------------------------------------------------------------- */

static void wifi_start_task(void *arg)
{
    (void)arg;

    /* Let the already-working KOYODA UI/audio owner settle first. */
    vTaskDelay(pdMS_TO_TICKS(WIFI_START_DELAY_MS));

    esp_err_t ret = init_nvs_safe();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(ret));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL)
    {
        ESP_LOGE(TAG, "Could not create Wi-Fi netifs");
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_storage failed: %s", esp_err_to_name(ret));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    s_test_events = xEventGroupCreate();
    if (s_test_events == NULL)
    {
        ESP_LOGE(TAG, "Could not allocate Wi-Fi event group");
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    ret = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_event_handler,
        NULL,
        &s_wifi_handler);
    if (ret == ESP_OK)
    {
        ret = esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_event_handler,
            NULL,
            &s_ip_handler);
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Wi-Fi event registration failed: %s", esp_err_to_name(ret));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    char ssid[33] = {0};
    char password[65] = {0};
    bool have_credentials = load_saved_credentials(
        ssid, sizeof(ssid), password, sizeof(password));

    if (have_credentials)
    {
        ESP_LOGI(TAG, "Using Wi-Fi saved in NVS: %s", ssid);
    }
    else if (KOYODA_WIFI_SSID[0] != '\0')
    {
        strlcpy(ssid, KOYODA_WIFI_SSID, sizeof(ssid));
        strlcpy(password, KOYODA_WIFI_PASSWORD, sizeof(password));
        have_credentials = true;
        ESP_LOGI(TAG, "Using build Wi-Fi fallback");
    }

    remember_active_credentials(ssid, password);

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret == ESP_OK && have_credentials)
    {
        ret = apply_sta_config(ssid, password);
    }
    if (ret == ESP_OK)
    {
        ret = esp_wifi_start();
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Wi-Fi setup failed: %s", esp_err_to_name(ret));
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    s_wifi_ready = true;
    ESP_LOGI(TAG, "Background Wi-Fi enabled; NVS user switching available");

    if (!have_credentials)
    {
        ESP_LOGW(TAG, "No saved Wi-Fi; SAFE v1 stays offline until CHANGE WI-FI is pressed");
    }

    vTaskDelete(NULL);
}

esp_err_t koyoda_wifi_start(void)
{
    if (s_started)
    {
        return ESP_OK;
    }

    s_started = true;

    BaseType_t result = xTaskCreate(
        wifi_start_task,
        "wifi_start",
        4096,
        NULL,
        1,
        NULL);

    if (result != pdPASS)
    {
        s_started = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Startup scheduled; UI untouched");
    return ESP_OK;
}
