#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "lvgl.h"
#include "esp_heap_caps.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "esp_io_expander.h"

#include "pmu_bridge.h"
#include "koyoda_animation.h"
#include "koyoda_wifi.h"
#include "koyoda_audio_duplex.h"
#include "koyoda_backend.h"
#include "koyoda_face_state.h"
#include "koyoda_ai_overlays.h"
#include "koyoda_charge_composite.h"
#include "koyoda_listening_notice.h"

LV_IMAGE_DECLARE(koyoda_idle);
LV_IMAGE_DECLARE(koyoda_sleep_1);
LV_IMAGE_DECLARE(koyoda_sleep_2);
LV_IMAGE_DECLARE(koyoda_sleep_3);

static const char *TAG = "KOYODA";

static lv_obj_t *face_img = NULL;
static lv_obj_t *thinking_overlay_img = NULL;
static lv_obj_t *speaking_overlay_img = NULL;
static lv_obj_t *listening_notice_overlay = NULL;
static lv_obj_t *power_overlay = NULL;
static lv_obj_t *battery_page = NULL;
static lv_obj_t *battery_fill = NULL;
static lv_obj_t *battery_percent_label = NULL;
static lv_obj_t *battery_status_label = NULL;
static lv_obj_t *battery_voltage_label = NULL;

static lv_obj_t *wifi_page = NULL;
static lv_obj_t *wifi_status_label = NULL;
static lv_obj_t *wifi_detail_label = NULL;
static lv_obj_t *wifi_rssi_label = NULL;
static lv_obj_t *wifi_change_button = NULL;
static lv_obj_t *wifi_change_button_label = NULL;
static lv_obj_t *wifi_signal_bars[4] = {NULL, NULL, NULL, NULL};

static lv_obj_t *volume_page = NULL;
static lv_obj_t *volume_percent_label = NULL;
static lv_obj_t *volume_status_label = NULL;

static lv_obj_t *swipe_layer = NULL;

static esp_io_expander_handle_t io_expander = NULL;

static volatile bool power_dialog_open = false;
static volatile bool power_dialog_requested = false;
static volatile bool battery_refresh_requested = false;
static volatile bool wifi_refresh_requested = false;
static uint32_t wifi_last_refresh_ms = 0;

/* Real AXP2101 charging-event state.
 * The animation is triggered on a false -> true charging transition.
 * If the user is on another page, the animation waits until FACE is visible.
 */
/* Animation and pending event are protected by the BSP display lock. */
static bool charging_animation_pending;
static koyoda_animation_t animation;
static bool touch_held;


/* AI-07 face animation runtime. LVGL is still owned only by this UI loop. */
static koyoda_face_ai_state_t ai_face_prev_state = KOYODA_FACE_AI_IDLE;
static unsigned ai_face_step = 0;
static uint32_t ai_face_frame_started_ms = 0;

/* Listening/ready indicator double-blink state. */
static bool listening_notice_active_prev = false;
static bool listening_notice_visible_prev = false;
static unsigned listening_notice_step = 0;
static uint32_t listening_notice_started_ms = 0;
static int charge_rendered_step = -1;
static int blink_rendered_frame = -1;
static void request_charging_animation(void)
{
    bsp_display_lock(-1);
    charging_animation_pending = true;
    bsp_display_unlock();
}

/* =========================================================
 * Page navigation
 *
 * PAGE_FUTURE is deliberately reserved now.  The enabled page
 * count stays at 2, so a second left swipe from Battery does
 * nothing yet.  Later we can enable PAGE_FUTURE without
 * rewriting the swipe system.
 * ========================================================= */

typedef enum
{
    PAGE_FACE = 0,
    PAGE_BATTERY,
    PAGE_WIFI,
    PAGE_VOLUME,
    PAGE_COUNT
} koyoda_page_t;

#define KOYODA_ENABLED_PAGE_COUNT 4
#define KOYODA_SWIPE_THRESHOLD_PX 70

static volatile koyoda_page_t current_page = PAGE_FACE;
static lv_point_t swipe_start = {0, 0};
static bool swipe_tracking = false;

/*
 * AI trigger:
 * - AI is OFF every boot.
 * - Hold the FACE for 1.2 seconds without moving more than 30 px.
 * - Long-press toggles AI ON/OFF.
 *
 * This is deliberately handled by the existing transparent swipe layer so no
 * second touch object is added on top of the face.
 */
#define KOYODA_AI_LONG_PRESS_MS       1200U
#define KOYODA_AI_LONG_PRESS_MOVE_PX    30

static uint32_t ai_press_started_ms = 0U;
static bool ai_long_press_fired = false;

/* Wi-Fi and Volume can sit above the transparent global swipe layer while active,
 * so both reuse this same swipe handler directly. */
static void swipe_event_cb(lv_event_t *e);

/* =========================================================
 * Face
 * ========================================================= */

/* =========================================================
 * Battery UI
 * ========================================================= */

static void update_battery_ui_locked(const pmu_battery_status_t *status, bool valid)
{
    if (battery_page == NULL)
    {
        return;
    }

    if (!valid)
    {
        lv_label_set_text(battery_percent_label, "--%");
        lv_label_set_text(battery_status_label, "Battery unavailable");
        lv_label_set_text(battery_voltage_label, "");
        lv_obj_add_flag(battery_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(battery_status_label, lv_color_hex(0x888888), 0);
        return;
    }

    if (!status->battery_connected)
    {
        lv_label_set_text(battery_percent_label, "--%");
        lv_obj_add_flag(battery_fill, LV_OBJ_FLAG_HIDDEN);

        if (status->vbus_in)
        {
            lv_label_set_text(battery_status_label, "USB Power");
            lv_label_set_text(battery_voltage_label, "No Battery");
            lv_obj_set_style_text_color(battery_status_label, lv_color_hex(0x00D5D5), 0);
        }
        else
        {
            lv_label_set_text(battery_status_label, "No Battery");
            lv_label_set_text(battery_voltage_label, "");
            lv_obj_set_style_text_color(battery_status_label, lv_color_hex(0x888888), 0);
        }

        return;
    }

    int percent = status->battery_percent;
    if (percent < 0)
    {
        percent = 0;
        lv_label_set_text(battery_percent_label, "--%");
    }
    else
    {
        if (percent > 100)
        {
            percent = 100;
        }

        char percent_text[16];
        snprintf(percent_text, sizeof(percent_text), "%d%%", percent);
        lv_label_set_text(battery_percent_label, percent_text);
    }

    lv_obj_clear_flag(battery_fill, LV_OBJ_FLAG_HIDDEN);

    int fill_width = (190 * percent) / 100;
    if (fill_width < 4)
    {
        fill_width = 4;
    }
    lv_obj_set_width(battery_fill, fill_width);

    /* If external power is present while a battery is connected,
       present the state as Charging, even if the PMIC has already
       tapered/stopped charge current at full capacity. */
    if (status->vbus_in)
    {
        lv_label_set_text(battery_status_label, "Charging");
        lv_obj_set_style_text_color(battery_status_label, lv_color_hex(0xFF7FA3), 0);
    }
    else
    {
        lv_label_set_text(battery_status_label, "Battery");
        lv_obj_set_style_text_color(battery_status_label, lv_color_hex(0x00D5D5), 0);
    }

    char voltage_text[24];
    snprintf(
        voltage_text,
        sizeof(voltage_text),
        "%u.%03u V",
        (unsigned)(status->battery_voltage_mv / 1000),
        (unsigned)(status->battery_voltage_mv % 1000));
    lv_label_set_text(battery_voltage_label, voltage_text);
}

static void battery_status_task(void *arg)
{
    (void)arg;

    /*
     * Wi-Fi can make the PMIC charging bit chatter briefly because power draw
     * changes when the radio starts.  Do NOT use status.charging as the event
     * source anymore.
     *
     * Instead, detect a real USB/VBUS insertion with a 3-sample debounce.
     * At 500 ms polling this means VBUS must be stable for about 1.5 seconds
     * before KOYODA queues the charging animation.
     */
    bool vbus_state_known = false;
    bool stable_vbus = false;
    bool candidate_vbus = false;
    unsigned candidate_count = 0;

    while (1)
    {
        pmu_battery_status_t status = {0};
        bool valid = (pmu_bridge_get_battery_status(&status) == 0);

        if (valid)
        {
            bool vbus_now = status.vbus_in;

            if (!vbus_state_known)
            {
                if (candidate_count == 0 || candidate_vbus != vbus_now)
                {
                    candidate_vbus = vbus_now;
                    candidate_count = 1;
                }
                else
                {
                    candidate_count++;
                }

                if (candidate_count >= 3)
                {
                    vbus_state_known = true;
                    stable_vbus = candidate_vbus;
                    candidate_count = 0;

                    /* Booted with USB/VBUS already present: play once only. */
                    if (stable_vbus)
                    {
                        request_charging_animation();
                        ESP_LOGI(TAG, "Stable VBUS detected at boot");
                    }
                }
            }
            else if (vbus_now != stable_vbus)
            {
                if (candidate_count == 0 || candidate_vbus != vbus_now)
                {
                    candidate_vbus = vbus_now;
                    candidate_count = 1;
                }
                else
                {
                    candidate_count++;
                }

                if (candidate_count >= 3)
                {
                    stable_vbus = candidate_vbus;
                    candidate_count = 0;

                    if (stable_vbus)
                    {
                        request_charging_animation();
                        koyoda_audio_duplex_beep_charge();
                        ESP_LOGI(TAG, "Stable VBUS inserted");
                    }
                    else
                    {
                        ESP_LOGI(TAG, "Stable VBUS removed");
                    }
                }
            }
            else
            {
                candidate_count = 0;
            }
        }

        /*
         * Battery UI only needs repainting when visible (or explicitly
         * requested), but charge detection above keeps running everywhere.
         */
        if (current_page == PAGE_BATTERY || battery_refresh_requested)
        {
            bsp_display_lock(-1);
            update_battery_ui_locked(&status, valid);
            bsp_display_unlock();

            if (valid)
            {
                ESP_LOGI(
                    TAG,
                    "Battery: present=%d vbus=%d charging=%d percent=%d voltage=%umV",
                    status.battery_connected,
                    status.vbus_in,
                    status.charging,
                    status.battery_percent,
                    status.battery_voltage_mv);
            }

            battery_refresh_requested = false;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void create_battery_page(lv_obj_t *screen)
{
    battery_page = lv_obj_create(screen);
    lv_obj_set_size(battery_page, 466, 466);
    lv_obj_center(battery_page);
    lv_obj_set_style_bg_color(battery_page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(battery_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_page, 0, 0);
    lv_obj_set_style_pad_all(battery_page, 0, 0);
    lv_obj_set_style_radius(battery_page, 0, 0);
    lv_obj_clear_flag(battery_page, LV_OBJ_FLAG_SCROLLABLE);

    /* Same confirmed physical orientation as the face assets. */
    lv_obj_set_style_transform_pivot_x(battery_page, 233, 0);
    lv_obj_set_style_transform_pivot_y(battery_page, 233, 0);
    lv_obj_set_style_transform_rotation(battery_page, 900, 0);

    lv_obj_t *title = lv_label_create(battery_page);
    lv_label_set_text(title, "BATTERY");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);

    /* Pixel-pet inspired battery: white shell + KOYODA cyan fill. */
    lv_obj_t *battery_body = lv_obj_create(battery_page);
    lv_obj_set_size(battery_body, 230, 108);
    lv_obj_align(battery_body, LV_ALIGN_CENTER, -10, -20);
    lv_obj_set_style_bg_color(battery_body, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(battery_body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_body, 8, 0);
    lv_obj_set_style_border_color(battery_body, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(battery_body, 16, 0);
    lv_obj_set_style_pad_all(battery_body, 10, 0);
    lv_obj_clear_flag(battery_body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *battery_tip = lv_obj_create(battery_page);
    lv_obj_set_size(battery_tip, 20, 48);
    lv_obj_align_to(battery_tip, battery_body, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    lv_obj_set_style_bg_color(battery_tip, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(battery_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_tip, 0, 0);
    lv_obj_set_style_radius(battery_tip, 5, 0);
    lv_obj_clear_flag(battery_tip, LV_OBJ_FLAG_SCROLLABLE);

    battery_fill = lv_obj_create(battery_body);
    lv_obj_set_size(battery_fill, 190, 72);
    lv_obj_align(battery_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(battery_fill, lv_color_hex(0x00D5D5), 0);
    lv_obj_set_style_bg_opa(battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_fill, 0, 0);
    lv_obj_set_style_radius(battery_fill, 8, 0);
    lv_obj_clear_flag(battery_fill, LV_OBJ_FLAG_SCROLLABLE);

    battery_percent_label = lv_label_create(battery_page);
    lv_label_set_text(battery_percent_label, "--%");
    lv_obj_set_style_text_color(battery_percent_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(battery_percent_label, &lv_font_montserrat_14, 0);
    lv_obj_align(battery_percent_label, LV_ALIGN_CENTER, 0, 70);

    battery_status_label = lv_label_create(battery_page);
    lv_label_set_text(battery_status_label, "Battery");
    lv_obj_set_style_text_color(battery_status_label, lv_color_hex(0x00D5D5), 0);
    lv_obj_set_style_text_font(battery_status_label, &lv_font_montserrat_14, 0);
    lv_obj_align(battery_status_label, LV_ALIGN_CENTER, 0, 102);

    battery_voltage_label = lv_label_create(battery_page);
    lv_label_set_text(battery_voltage_label, "");
    lv_obj_set_style_text_color(battery_voltage_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(battery_voltage_label, &lv_font_montserrat_14, 0);
    lv_obj_align(battery_voltage_label, LV_ALIGN_CENTER, 0, 132);

    lv_obj_add_flag(battery_page, LV_OBJ_FLAG_HIDDEN);
}

/* =========================================================
 * Wi-Fi Status UI
 *
 * Important: the Wi-Fi networking task still never touches LVGL.
 * This page only READS the thread-safe status getters from the
 * existing background Wi-Fi module.
 * ========================================================= */

static void set_wifi_bar_level_locked(int bars)
{
    for (int i = 0; i < 4; ++i)
    {
        if (wifi_signal_bars[i] == NULL)
        {
            continue;
        }

        lv_obj_set_style_bg_color(
            wifi_signal_bars[i],
            (i < bars) ? lv_color_hex(0x00D5D5) : lv_color_hex(0x303030),
            0);
    }
}

static void update_wifi_ui_locked(void)
{
    if (wifi_page == NULL)
    {
        return;
    }

    const bool connected = koyoda_wifi_is_connected();
    const int rssi = koyoda_wifi_get_rssi();
    const bool provisioning = koyoda_wifi_is_provisioning();
    const koyoda_wifi_setup_state_t setup_state = koyoda_wifi_get_setup_state();

    if (provisioning)
    {
        set_wifi_bar_level_locked(0);

        if (setup_state == KOYODA_WIFI_SETUP_TESTING)
        {
            lv_label_set_text(wifi_status_label, "TESTING...");
            lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0xFFD166), 0);
            lv_label_set_text(wifi_detail_label, "Trying new Wi-Fi");
            lv_label_set_text(wifi_rssi_label, "Keep phone connected");
        }
        else if (setup_state == KOYODA_WIFI_SETUP_FAILED)
        {
            lv_label_set_text(wifi_status_label, "TRY AGAIN");
            lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0xFF7FA3), 0);
            lv_label_set_text(wifi_detail_label, "Join KOYODA-Setup");
            lv_label_set_text(wifi_rssi_label, "PW: koyoda88");
        }
        else if (setup_state == KOYODA_WIFI_SETUP_SUCCESS)
        {
            lv_label_set_text(wifi_status_label, "SAVED");
            lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x00D5D5), 0);
            lv_label_set_text(wifi_detail_label, "New Wi-Fi connected");
            lv_label_set_text(wifi_rssi_label, "Setup closing...");
        }
        else
        {
            lv_label_set_text(wifi_status_label, "SETUP MODE");
            lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x00D5D5), 0);
            lv_label_set_text(wifi_detail_label, "Join KOYODA-Setup");
            lv_label_set_text(wifi_rssi_label, "PW: koyoda88");
        }

        if (wifi_change_button_label != NULL)
        {
            lv_label_set_text(wifi_change_button_label, "SETUP ACTIVE");
        }
        return;
    }

    if (wifi_change_button_label != NULL)
    {
        lv_label_set_text(wifi_change_button_label, "CHANGE WI-FI");
    }

    if (!connected)
    {
        lv_label_set_text(wifi_status_label, "CONNECTING...");
        lv_obj_set_style_text_color(
            wifi_status_label,
            lv_color_hex(0xFF7FA3),
            0);

        lv_label_set_text(
            wifi_detail_label,
            "Waiting for Wi-Fi");

        lv_label_set_text(
            wifi_rssi_label,
            "RSSI -- dBm");

        set_wifi_bar_level_locked(0);
        return;
    }

    /* Connected becomes true only after DHCP succeeds. */
    lv_label_set_text(wifi_status_label, "CONNECTED");
    lv_obj_set_style_text_color(
        wifi_status_label,
        lv_color_hex(0x00D5D5),
        0);

    char ssid[33];
    koyoda_wifi_get_ssid(ssid, sizeof(ssid));
    lv_label_set_text(
        wifi_detail_label,
        ssid[0] ? ssid : "IP acquired");

    char rssi_text[32];
    snprintf(
        rssi_text,
        sizeof(rssi_text),
        "RSSI %d dBm",
        rssi);
    lv_label_set_text(wifi_rssi_label, rssi_text);

    int bars = 1;
    if (rssi >= -55)
    {
        bars = 4;
    }
    else if (rssi >= -67)
    {
        bars = 3;
    }
    else if (rssi >= -75)
    {
        bars = 2;
    }

    set_wifi_bar_level_locked(bars);
}

static void wifi_change_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    esp_err_t ret = koyoda_wifi_begin_provisioning();
    if (ret == ESP_OK)
    {
        wifi_refresh_requested = true;
        ESP_LOGI(TAG, "Phone Wi-Fi setup requested");
    }
    else
    {
        ESP_LOGW(TAG, "Wi-Fi setup request failed: %s", esp_err_to_name(ret));
    }
}

static void create_wifi_page(lv_obj_t *screen)
{
    wifi_page = lv_obj_create(screen);
    lv_obj_set_size(wifi_page, 466, 466);
    lv_obj_center(wifi_page);
    lv_obj_set_style_bg_color(wifi_page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(wifi_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_page, 0, 0);
    lv_obj_set_style_pad_all(wifi_page, 0, 0);
    lv_obj_set_style_radius(wifi_page, 0, 0);
    lv_obj_clear_flag(wifi_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wifi_page, LV_OBJ_FLAG_CLICKABLE);

    /* Same confirmed 90-degree physical orientation as Face/Battery. */
    lv_obj_set_style_transform_pivot_x(wifi_page, 233, 0);
    lv_obj_set_style_transform_pivot_y(wifi_page, 233, 0);
    lv_obj_set_style_transform_rotation(wifi_page, 900, 0);

    lv_obj_t *title = lv_label_create(wifi_page);
    lv_label_set_text(title, "WI-FI");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 72);

    /* Simple four-bar signal meter. */
    const int heights[4] = {22, 38, 54, 70};
    for (int i = 0; i < 4; ++i)
    {
        wifi_signal_bars[i] = lv_obj_create(wifi_page);
        lv_obj_set_size(wifi_signal_bars[i], 24, heights[i]);
        lv_obj_set_style_bg_color(
            wifi_signal_bars[i],
            lv_color_hex(0x303030),
            0);
        lv_obj_set_style_bg_opa(
            wifi_signal_bars[i],
            LV_OPA_COVER,
            0);
        lv_obj_set_style_border_width(
            wifi_signal_bars[i],
            0,
            0);
        lv_obj_set_style_radius(
            wifi_signal_bars[i],
            5,
            0);
        lv_obj_clear_flag(
            wifi_signal_bars[i],
            LV_OBJ_FLAG_SCROLLABLE);

        /* Bottom-align the bars around the page center. */
        lv_obj_align(
            wifi_signal_bars[i],
            LV_ALIGN_CENTER,
            -69 + (i * 46),
            -28 + ((70 - heights[i]) / 2));
    }

    wifi_status_label = lv_label_create(wifi_page);
    lv_label_set_text(wifi_status_label, "CONNECTING...");
    lv_obj_set_style_text_color(
        wifi_status_label,
        lv_color_hex(0xFF7FA3),
        0);
    lv_obj_set_style_text_font(
        wifi_status_label,
        &lv_font_montserrat_14,
        0);
    lv_obj_align(
        wifi_status_label,
        LV_ALIGN_CENTER,
        0,
        70);

    wifi_detail_label = lv_label_create(wifi_page);
    lv_label_set_text(wifi_detail_label, "Waiting for Wi-Fi");
    lv_obj_set_style_text_color(
        wifi_detail_label,
        lv_color_hex(0xFFFFFF),
        0);
    lv_obj_set_style_text_font(
        wifi_detail_label,
        &lv_font_montserrat_14,
        0);
    lv_obj_align(
        wifi_detail_label,
        LV_ALIGN_CENTER,
        0,
        102);

    wifi_rssi_label = lv_label_create(wifi_page);
    lv_label_set_text(wifi_rssi_label, "RSSI -- dBm");
    lv_obj_set_style_text_color(
        wifi_rssi_label,
        lv_color_hex(0x888888),
        0);
    lv_obj_set_style_text_font(
        wifi_rssi_label,
        &lv_font_montserrat_14,
        0);
    lv_obj_align(
        wifi_rssi_label,
        LV_ALIGN_CENTER,
        0,
        126);

    wifi_change_button = lv_button_create(wifi_page);
    lv_obj_set_size(wifi_change_button, 190, 54);
    lv_obj_align(wifi_change_button, LV_ALIGN_CENTER, 0, 174);
    lv_obj_set_style_radius(wifi_change_button, 18, 0);
    lv_obj_set_style_bg_color(wifi_change_button, lv_color_hex(0x17272D), 0);
    lv_obj_set_style_border_width(wifi_change_button, 2, 0);
    lv_obj_set_style_border_color(wifi_change_button, lv_color_hex(0x00D5D5), 0);
    lv_obj_add_flag(wifi_change_button, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(wifi_change_button, wifi_change_button_cb, LV_EVENT_CLICKED, NULL);

    wifi_change_button_label = lv_label_create(wifi_change_button);
    lv_label_set_text(wifi_change_button_label, "CHANGE WI-FI");
    lv_obj_set_style_text_color(wifi_change_button_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(wifi_change_button_label, &lv_font_montserrat_14, 0);
    lv_obj_center(wifi_change_button_label);

    /* Wi-Fi page moves above the transparent swipe layer while visible so
     * the setup button receives touch. It therefore owns swipe input too. */
    lv_obj_add_event_cb(wifi_page, swipe_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(wifi_page, swipe_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(wifi_page, swipe_event_cb, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_add_flag(wifi_page, LV_OBJ_FLAG_HIDDEN);
}

/* =========================================================
 * Volume UI
 *
 * Page order:
 *   Face -> Battery -> Wi-Fi -> Volume
 *
 * Controls:
 *   - / + : 5% steps, 0..100
 *   TEST  : one short beep at the selected volume
 *
 * The speaker module stores the selected value in NVS.
 * ========================================================= */

#define KOYODA_VOLUME_STEP 5

static void update_volume_ui_locked(void)
{
    if (volume_page == NULL)
    {
        return;
    }

    int volume = koyoda_audio_duplex_get_volume();

    char text[16];
    snprintf(text, sizeof(text), "%d%%", volume);
    lv_label_set_text(volume_percent_label, text);

    if (volume == 0)
    {
        lv_label_set_text(volume_status_label, "MUTED");
        lv_obj_set_style_text_color(
            volume_status_label,
            lv_color_hex(0x888888),
            0);
    }
    else
    {
        lv_label_set_text(volume_status_label, "SPEAKER");
        lv_obj_set_style_text_color(
            volume_status_label,
            lv_color_hex(0x00D5D5),
            0);
    }
}

static void volume_minus_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    int volume = koyoda_audio_duplex_get_volume() - KOYODA_VOLUME_STEP;
    if (volume < 0)
    {
        volume = 0;
    }

    koyoda_audio_duplex_set_volume(volume);
    update_volume_ui_locked();

    ESP_LOGI(TAG, "Volume -> %d%%", volume);
}

static void volume_plus_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    int volume = koyoda_audio_duplex_get_volume() + KOYODA_VOLUME_STEP;
    if (volume > 100)
    {
        volume = 100;
    }

    koyoda_audio_duplex_set_volume(volume);
    update_volume_ui_locked();

    ESP_LOGI(TAG, "Volume -> %d%%", volume);
}

static void volume_test_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    koyoda_audio_duplex_beep_test();
    ESP_LOGI(
        TAG,
        "Volume test requested at %d%%",
        koyoda_audio_duplex_get_volume());
}

static lv_obj_t *create_volume_button(
    lv_obj_t *parent,
    const char *label_text,
    int width,
    int height,
    int x_offset,
    int y_offset,
    lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_align(button, LV_ALIGN_CENTER, x_offset, y_offset);
    lv_obj_set_style_radius(button, 20, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x00D5D5), 0);

    /*
     * Let press/release events bubble to volume_page so a swipe that starts
     * on a button can still navigate normally.
     */
    lv_obj_add_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_add_event_cb(
        button,
        callback,
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_center(label);

    return button;
}

static void create_volume_page(lv_obj_t *screen)
{
    volume_page = lv_obj_create(screen);
    lv_obj_set_size(volume_page, 466, 466);
    lv_obj_center(volume_page);
    lv_obj_set_style_bg_color(volume_page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(volume_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(volume_page, 0, 0);
    lv_obj_set_style_pad_all(volume_page, 0, 0);
    lv_obj_set_style_radius(volume_page, 0, 0);
    lv_obj_clear_flag(volume_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(volume_page, LV_OBJ_FLAG_CLICKABLE);

    /* Same confirmed physical orientation as Face/Battery/Wi-Fi. */
    lv_obj_set_style_transform_pivot_x(volume_page, 233, 0);
    lv_obj_set_style_transform_pivot_y(volume_page, 233, 0);
    lv_obj_set_style_transform_rotation(volume_page, 900, 0);

    lv_obj_t *title = lv_label_create(volume_page);
    lv_label_set_text(title, "VOLUME");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 72);

    lv_obj_t *sound_label = lv_label_create(volume_page);
    lv_label_set_text(sound_label, "SOUND");
    lv_obj_set_style_text_color(sound_label, lv_color_hex(0x00D5D5), 0);
    lv_obj_set_style_text_font(sound_label, &lv_font_montserrat_14, 0);
    lv_obj_align(sound_label, LV_ALIGN_CENTER, 0, -86);

    volume_percent_label = lv_label_create(volume_page);
    lv_label_set_text(volume_percent_label, "90%");
    lv_obj_set_style_text_color(
        volume_percent_label,
        lv_color_hex(0xFFFFFF),
        0);
    lv_obj_set_style_text_font(
        volume_percent_label,
        &lv_font_montserrat_14,
        0);
    lv_obj_align(
        volume_percent_label,
        LV_ALIGN_CENTER,
        0,
        -46);

    create_volume_button(
        volume_page,
        "-",
        100,
        70,
        -75,
        18,
        volume_minus_button_cb);

    create_volume_button(
        volume_page,
        "+",
        100,
        70,
        75,
        18,
        volume_plus_button_cb);

    create_volume_button(
        volume_page,
        "TEST",
        190,
        62,
        0,
        106,
        volume_test_button_cb);

    volume_status_label = lv_label_create(volume_page);
    lv_label_set_text(volume_status_label, "SPEAKER");
    lv_obj_set_style_text_color(
        volume_status_label,
        lv_color_hex(0x00D5D5),
        0);
    lv_obj_set_style_text_font(
        volume_status_label,
        &lv_font_montserrat_14,
        0);
    lv_obj_align(
        volume_status_label,
        LV_ALIGN_CENTER,
        0,
        156);

    /*
     * While Volume is visible, this page is moved above the transparent
     * swipe layer. Therefore it must own swipe input itself.
     */
    lv_obj_add_event_cb(volume_page, swipe_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(volume_page, swipe_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(volume_page, swipe_event_cb, LV_EVENT_PRESS_LOST, NULL);

    update_volume_ui_locked();

    lv_obj_add_flag(volume_page, LV_OBJ_FLAG_HIDDEN);
}

/* Called only from an LVGL event callback, so do not take the BSP LVGL lock here. */
static void set_page_from_lvgl(koyoda_page_t page)
{
    if (page < PAGE_FACE || page >= KOYODA_ENABLED_PAGE_COUNT)
    {
        return;
    }

    current_page = page;

    if (animation.mode == ANIM_CHARGE)
    {
        charging_animation_pending = true;
    }

    anim_reset(&animation, lv_tick_get());

    /*
     * Hard mutual exclusion:
     * hide every content page first, then show exactly one.
     * This prevents the Face/Battery/Wi-Fi overlay bug by construction.
     */
    lv_obj_add_flag(face_img, LV_OBJ_FLAG_HIDDEN);
    if (thinking_overlay_img) lv_obj_add_flag(thinking_overlay_img, LV_OBJ_FLAG_HIDDEN);
    if (speaking_overlay_img) lv_obj_add_flag(speaking_overlay_img, LV_OBJ_FLAG_HIDDEN);
    if (listening_notice_overlay) lv_obj_add_flag(listening_notice_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(battery_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(volume_page, LV_OBJ_FLAG_HIDDEN);

    if (page == PAGE_FACE)
    {
        lv_obj_clear_flag(face_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(face_img, &koyoda_idle);
        ESP_LOGI(TAG, "Page -> FACE");
    }
    else if (page == PAGE_BATTERY)
    {
        lv_obj_clear_flag(battery_page, LV_OBJ_FLAG_HIDDEN);
        battery_refresh_requested = true;
        ESP_LOGI(TAG, "Page -> BATTERY");
    }
    else if (page == PAGE_WIFI)
    {
        lv_obj_clear_flag(wifi_page, LV_OBJ_FLAG_HIDDEN);
        wifi_refresh_requested = true;
        lv_obj_move_foreground(wifi_page);
        /* A Wi-Fi connect/channel transition can leave stale pixels in the
         * panel buffer even though LVGL itself is still responsive. Repaint
         * the whole screen only when the user changes page; do not poll Wi-Fi
         * state from the animation loop. */
        lv_obj_invalidate(lv_screen_active());
        ESP_LOGI(TAG, "Page -> WIFI");
        return;
    }
    else if (page == PAGE_VOLUME)
    {
        lv_obj_clear_flag(volume_page, LV_OBJ_FLAG_HIDDEN);
        update_volume_ui_locked();

        /*
         * Put the interactive Volume page above the transparent global
         * swipe layer so its - / + / TEST buttons receive touch.
         */
        lv_obj_move_foreground(volume_page);
        lv_obj_invalidate(lv_screen_active());

        ESP_LOGI(TAG, "Page -> VOLUME");
        return;
    }

    /*
     * On Face/Battery the existing transparent layer stays on top.
     * Wi-Fi and Volume return above after taking ownership of swipe input.
     */
    lv_obj_move_foreground(swipe_layer);
    lv_obj_invalidate(lv_screen_active());
}

static void navigate_next_from_lvgl(void)
{
    int next = (int)current_page + 1;

    if (next >= KOYODA_ENABLED_PAGE_COUNT)
    {
        return;
    }

    set_page_from_lvgl((koyoda_page_t)next);
}

static void navigate_previous_from_lvgl(void)
{
    int previous = (int)current_page - 1;

    if (previous < PAGE_FACE)
    {
        return;
    }

    set_page_from_lvgl((koyoda_page_t)previous);
}

static void swipe_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);

    if (indev == NULL || power_dialog_open)
    {
        swipe_tracking = false;
        ai_long_press_fired = false;
        return;
    }

    if (code == LV_EVENT_PRESSED)
    {
        touch_held = true;

        bool woke = anim_touch(&animation, lv_tick_get());
        if (woke)
        {
            /*
             * First touch only wakes KOYODA.  It never also navigates or
             * toggles AI.
             */
            swipe_tracking = false;
            ai_long_press_fired = false;
            ESP_LOGI(TAG, "Wake transition started");
            return;
        }

        lv_indev_get_point(indev, &swipe_start);
        swipe_tracking = true;
        ai_press_started_ms = lv_tick_get();
        ai_long_press_fired = false;
        return;
    }

    /*
     * Manual long-press detection gives us a deliberate 1.2 s threshold
     * instead of depending on LVGL's global/default long-press time.
     *
     * It only works on PAGE_FACE and only when the finger has stayed nearly
     * stationary, so a slow swipe will not accidentally toggle AI.
     */
    if (code == LV_EVENT_PRESSING &&
        current_page == PAGE_FACE &&
        swipe_tracking &&
        !ai_long_press_fired)
    {
        lv_point_t now_point = {0, 0};
        lv_indev_get_point(indev, &now_point);

        int dx =
            (int)now_point.x - (int)swipe_start.x;
        int dy =
            (int)now_point.y - (int)swipe_start.y;

        if (abs(dx) <= KOYODA_AI_LONG_PRESS_MOVE_PX &&
            abs(dy) <= KOYODA_AI_LONG_PRESS_MOVE_PX &&
            (uint32_t)(lv_tick_get() - ai_press_started_ms) >=
                KOYODA_AI_LONG_PRESS_MS)
        {
            const bool new_enabled =
                !koyoda_audio_duplex_ai_is_enabled();

            koyoda_audio_duplex_ai_set_enabled(new_enabled);

            /*
             * Never let a long-press release become a swipe.
             * The audio module provides:
             *   ON  -> one confirmation beep
             *   OFF -> two confirmation beeps
             */
            ai_long_press_fired = true;
            swipe_tracking = false;

            anim_reset(&animation, lv_tick_get());

            ESP_LOGI(
                TAG,
                "AI MODE -> %s (face long-press)",
                new_enabled ? "ON" : "OFF");
        }

        return;
    }

    if (code == LV_EVENT_RELEASED ||
        code == LV_EVENT_PRESS_LOST)
    {
        touch_held = false;
        anim_touch(&animation, lv_tick_get());
    }

    if (code == LV_EVENT_PRESS_LOST)
    {
        swipe_tracking = false;
        ai_long_press_fired = false;
        return;
    }

    if (code != LV_EVENT_RELEASED)
    {
        return;
    }

    /*
     * A completed long-press is consumed here.  No page navigation happens
     * when the finger is released.
     */
    if (ai_long_press_fired)
    {
        ai_long_press_fired = false;
        swipe_tracking = false;
        return;
    }

    if (!swipe_tracking)
    {
        return;
    }

    lv_point_t end = {0, 0};
    lv_indev_get_point(indev, &end);
    swipe_tracking = false;

    int dx = (int)end.x - (int)swipe_start.x;
    int dy = (int)end.y - (int)swipe_start.y;
    int abs_dx = abs(dx);
    int abs_dy = abs(dy);

    if (abs_dx < KOYODA_SWIPE_THRESHOLD_PX &&
        abs_dy < KOYODA_SWIPE_THRESHOLD_PX)
    {
        return;
    }

    /* The confirmed UI is rotated 90 degrees on this board.  Depending on
       whether the BSP has already rotated touch coordinates, a physical
       left/right swipe can arrive on either the X or Y axis.  Supporting
       the dominant axis keeps navigation correct in both cases.

       Physical LEFT  -> negative dominant delta -> next page
       Physical RIGHT -> positive dominant delta -> previous page
     */
    int dominant_delta =
        (abs_dy >= abs_dx) ? dy : dx;

    if (dominant_delta < 0)
    {
        navigate_next_from_lvgl();
    }
    else
    {
        navigate_previous_from_lvgl();
    }
}

static void create_swipe_layer(lv_obj_t *screen)
{
    swipe_layer = lv_obj_create(screen);
    lv_obj_set_size(swipe_layer, 466, 466);
    lv_obj_center(swipe_layer);
    lv_obj_set_style_bg_opa(swipe_layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(swipe_layer, 0, 0);
    lv_obj_set_style_pad_all(swipe_layer, 0, 0);
    lv_obj_set_style_radius(swipe_layer, 0, 0);
    lv_obj_clear_flag(swipe_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(swipe_layer, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(swipe_layer, swipe_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(swipe_layer, swipe_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(swipe_layer, swipe_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(swipe_layer, swipe_event_cb, LV_EVENT_PRESS_LOST, NULL);
}

/* =========================================================
 * Power Dialog
 * ========================================================= */

static void close_power_dialog(void)
{
    if (power_overlay != NULL)
    {
        lv_obj_delete(power_overlay);
        power_overlay = NULL;
    }

    power_dialog_open = false;
    anim_touch(&animation, lv_tick_get());

    ESP_LOGI(TAG, "Power dialog closed");
}

static void cancel_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        ESP_LOGI(TAG, "Power off cancelled");
        close_power_dialog();
    }
}

static void actual_power_off_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "Sending shutdown command to AXP2101");
    pmu_bridge_shutdown();

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void power_off_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        ESP_LOGI(TAG, "POWER OFF requested");
        close_power_dialog();

        BaseType_t result = xTaskCreate(
            actual_power_off_task,
            "power_off",
            4096,
            NULL,
            6,
            NULL);

        if (result != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create power off task");
        }
    }
}

static void show_power_dialog(void)
{
    if (power_dialog_open)
    {
        return;
    }

    power_dialog_open = true;

    bsp_display_lock(-1);

    touch_held = false;
    swipe_tracking = false;
    anim_touch(&animation, lv_tick_get());
    power_overlay = lv_obj_create(lv_screen_active());

    lv_obj_set_style_transform_pivot_x(power_overlay, 233, 0);
    lv_obj_set_style_transform_pivot_y(power_overlay, 233, 0);
    lv_obj_set_style_transform_rotation(power_overlay, 900, 0);

    lv_obj_set_size(power_overlay, 466, 466);
    lv_obj_center(power_overlay);
    lv_obj_set_style_bg_color(power_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(power_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(power_overlay, 0, 0);
    lv_obj_set_style_pad_all(power_overlay, 0, 0);
    lv_obj_clear_flag(power_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(power_overlay);
    lv_obj_set_size(panel, 340, 220);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x181818), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(panel, 28, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Power off?");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    lv_obj_t *cancel_btn = lv_button_create(panel);
    lv_obj_set_size(cancel_btn, 125, 62);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 22, -25);
    lv_obj_set_style_radius(cancel_btn, 18, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x444444), 0);
    lv_obj_add_event_cb(cancel_btn, cancel_button_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(cancel_label);

    lv_obj_t *power_btn = lv_button_create(panel);
    lv_obj_set_size(power_btn, 145, 62);
    lv_obj_align(power_btn, LV_ALIGN_BOTTOM_RIGHT, -22, -25);
    lv_obj_set_style_radius(power_btn, 18, 0);
    lv_obj_set_style_bg_color(power_btn, lv_color_hex(0x9C2525), 0);
    lv_obj_add_event_cb(power_btn, power_off_button_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *power_label = lv_label_create(power_btn);
    lv_label_set_text(power_label, "Power Off");
    lv_obj_set_style_text_color(power_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(power_label);

    bsp_display_unlock();

    ESP_LOGI(TAG, "Power dialog shown");
}

/* =========================================================
 * Power Button Task
 * ========================================================= */

static void power_button_task(void *arg)
{
    (void)arg;
    bool was_pressed = false;
    bool long_press_reported = false;
    TickType_t press_start = 0;

    while (1)
    {
        uint32_t level = 0;

        esp_err_t err = esp_io_expander_get_level(
            io_expander,
            IO_EXPANDER_PIN_NUM_4,
            &level);

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to read Power button: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        bool pressed = (level & IO_EXPANDER_PIN_NUM_4) != 0;

        if (pressed && !was_pressed)
        {
            bsp_display_lock(-1);
            anim_touch(&animation, lv_tick_get());
            bsp_display_unlock();
            ESP_LOGI(TAG, "PWR pressed");
            press_start = xTaskGetTickCount();
            long_press_reported = false;
            was_pressed = true;
        }

        if (pressed && was_pressed && !long_press_reported)
        {
            TickType_t held_ticks = xTaskGetTickCount() - press_start;
            uint32_t held_ms = held_ticks * portTICK_PERIOD_MS;

            if (held_ms >= 2000)
            {
                ESP_LOGI(TAG, "PWR LONG PRESS detected");
                long_press_reported = true;
                /* Never create/delete LVGL objects from the GPIO polling task.
                 * Queue the request and let the main UI loop perform the
                 * display-locked LVGL work. */
                power_dialog_requested = true;
            }
        }

        if (!pressed && was_pressed)
        {
            ESP_LOGI(TAG, "PWR released");
            was_pressed = false;
            long_press_reported = false;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* =========================================================
 * Charging animation helpers
 * ========================================================= */

/* One task owns all face frames: blink, drowsy, sleep and charging.
 * This prevents two animation tasks from overwriting one another. */
static void face_animation_step(void)
{
    /* Power-button task is not allowed to touch LVGL directly. */
    if (power_dialog_requested)
    {
        power_dialog_requested = false;
        if (!power_dialog_open)
        {
            show_power_dialog();
        }
    }

    /*
     * BLINK LITE v4:
     * frame IDs 1/2 are composed into the shared PSRAM face frame below.
     * They no longer require full-screen koyoda_half/koyoda_closed assets.
     */
    const lv_image_dsc_t *normal_frames[] = {
        &koyoda_idle, &koyoda_idle, &koyoda_idle,
        &koyoda_sleep_1, &koyoda_sleep_2, &koyoda_sleep_3
    };

    /*
     * CHARGE LITE v2 uses the existing full-screen face object, not a
     * separate LVGL image overlay.  A single PSRAM work frame is composed
     * from koyoda_idle + the tiny mouth/electricity patch.
     */

    /*
     * AI-07 LITE:
     * The Astra-approved frames differ from idle only in tiny areas.
     * We animate only those cropped patches instead of compiling six
     * additional 466x466 RGB565 full-screen frames.
     */
    const lv_image_dsc_t *thinking_frames[] = {
        &koyoda_think_patch_01,
        &koyoda_think_patch_02,
        &koyoda_think_patch_03,
        &koyoda_think_patch_02,
    };
    const uint16_t thinking_ms[] = {300, 300, 380, 300};

    const lv_image_dsc_t *speaking_frames[] = {
        &koyoda_speak_patch_soft,
        &koyoda_speak_patch_open,
        &koyoda_speak_patch_soft,
        &koyoda_speak_patch_closed,
    };
    const uint16_t speaking_ms[] = {110, 135, 105, 80};

    /*
     * Listening notice: two quick flashes followed by a longer pause.
     * ON 140ms -> OFF 100ms -> ON 140ms -> OFF 820ms
     */
    const uint16_t listening_notice_ms[] = {140, 100, 140, 820};

    bsp_display_lock(-1);

    uint32_t now_ms = lv_tick_get();
    koyoda_face_ai_state_t ai_state = koyoda_face_state_get();

    if (ai_state != ai_face_prev_state)
    {
        ai_face_prev_state = ai_state;
        ai_face_step = 0;
        ai_face_frame_started_ms = now_ms;

        /* AI interaction counts as activity, so KOYODA does not fall asleep
         * immediately after finishing a reply. */
        anim_reset(&animation, now_ms);
    }

    const lv_image_dsc_t *desired_face = NULL;
    const lv_image_dsc_t *desired_thinking_patch = NULL;
    const lv_image_dsc_t *desired_speaking_patch = NULL;

    if (ai_state == KOYODA_FACE_AI_THINKING)
    {
        /* Keep the base face at idle and animate only the tiny dot strip. */
        anim_reset(&animation, now_ms);

        if ((uint32_t)(now_ms - ai_face_frame_started_ms) >=
            thinking_ms[ai_face_step])
        {
            ai_face_step = (ai_face_step + 1U) % 4U;
            ai_face_frame_started_ms = now_ms;
        }

        desired_face = &koyoda_idle;
        desired_thinking_patch = thinking_frames[ai_face_step];
    }
    else if (ai_state == KOYODA_FACE_AI_SPEAKING)
    {
        /* Keep the base face at idle and animate only the mouth patch. */
        anim_reset(&animation, now_ms);

        if ((uint32_t)(now_ms - ai_face_frame_started_ms) >=
            speaking_ms[ai_face_step])
        {
            ai_face_step = (ai_face_step + 1U) % 4U;
            ai_face_frame_started_ms = now_ms;
        }

        desired_face = &koyoda_idle;
        desired_speaking_patch = speaking_frames[ai_face_step];
    }
    else
    {
        unsigned frame = anim_tick(
            &animation,
            now_ms,
            current_page == PAGE_FACE,
            power_dialog_open,
            touch_held,
            &charging_animation_pending);

        desired_face = normal_frames[frame];

        /*
         * Charge LITE v2:
         * Keep the animation state/timing from koyoda_animation.h, but do
         * not create a rotated LVGL overlay.  Steps 1..4 are composed into
         * one full-screen PSRAM work frame and rendered through face_img,
         * the same path used by the historically stable full-screen faces.
         */
        if (animation.mode == ANIM_CHARGE &&
            animation.step >= 1U && animation.step <= 4U)
        {
            if ((int)animation.step != charge_rendered_step)
            {
                (void)koyoda_charge_composite_apply(animation.step);
                charge_rendered_step = (int)animation.step;
            }

            desired_face = koyoda_charge_composite_image(animation.step);
            blink_rendered_frame = -1;
        }
        else
        {
            charge_rendered_step = -1;

            /*
             * BLINK LITE v4:
             * half/closed are eye-only compact patches copied into the SAME
             * full-screen PSRAM work frame already used by charging.
             * No extra 466x466 allocation and no LVGL eye overlay objects.
             *
             * This also handles DROWSY and WAKE, because those modes use the
             * same frame IDs 1=half and 2=closed.
             */
            if (frame == 1U || frame == 2U)
            {
                if ((int)frame != blink_rendered_frame)
                {
                    (void)koyoda_blink_composite_apply(frame);
                    blink_rendered_frame = (int)frame;
                }

                desired_face = koyoda_blink_composite_image(frame);
            }
            else
            {
                blink_rendered_frame = -1;
            }
        }

        /*
         * Wake expression without a full-screen fun_happy asset:
         * during the last ANIM_WAKE step, keep the normal idle face and
         * reuse the already-compiled speak-open mouth overlay.
         */
        if (animation.mode == ANIM_WAKE && animation.step == 3U)
        {
            desired_speaking_patch = &koyoda_speak_patch_open;
        }
    }

    const bool face_visible =
        (current_page == PAGE_FACE && !power_dialog_open);

    /*
     * LISTENING / READY:
     * Show whenever the user has explicitly enabled AI and KOYODA is on the
     * normal Face idle state.  Do not depend on VAD starting first.
     *
     * THINKING/SPEAKING have their own face state and automatically suppress
     * this indicator. Charge, drowsy, sleep and wake also suppress it.
     */
    const bool listening_notice_active =
        face_visible &&
        koyoda_audio_duplex_ai_is_enabled() &&
        ai_state == KOYODA_FACE_AI_IDLE &&
        animation.mode == ANIM_IDLE;

    if (listening_notice_active != listening_notice_active_prev)
    {
        listening_notice_active_prev = listening_notice_active;
        listening_notice_step = 0U;
        listening_notice_started_ms = now_ms;
    }

    if (listening_notice_active &&
        (uint32_t)(now_ms - listening_notice_started_ms) >=
            listening_notice_ms[listening_notice_step])
    {
        listening_notice_step =
            (listening_notice_step + 1U) % 4U;
        listening_notice_started_ms = now_ms;
    }

    const bool listening_notice_should_show =
        listening_notice_active &&
        (listening_notice_step == 0U ||
         listening_notice_step == 2U);

    /*
     * Change LVGL visibility only when the requested state changes.
     * This avoids needless invalidation every 20 ms.
     */
    if (listening_notice_overlay != NULL &&
        listening_notice_should_show != listening_notice_visible_prev)
    {
        listening_notice_visible_prev = listening_notice_should_show;

        if (listening_notice_should_show)
        {
            lv_obj_clear_flag(
                listening_notice_overlay,
                LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(
                listening_notice_overlay,
                LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (face_visible &&
        desired_face != NULL &&
        lv_image_get_src(face_img) != desired_face)
    {
        lv_image_set_src(face_img, desired_face);
    }

    if (!face_visible || desired_thinking_patch == NULL)
    {
        lv_obj_add_flag(thinking_overlay_img, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        if (lv_image_get_src(thinking_overlay_img) != desired_thinking_patch)
        {
            lv_image_set_src(thinking_overlay_img, desired_thinking_patch);
        }
        lv_obj_clear_flag(thinking_overlay_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (!face_visible || desired_speaking_patch == NULL)
    {
        lv_obj_add_flag(speaking_overlay_img, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        if (lv_image_get_src(speaking_overlay_img) != desired_speaking_patch)
        {
            lv_image_set_src(speaking_overlay_img, desired_speaking_patch);
        }
        lv_obj_clear_flag(speaking_overlay_img, LV_OBJ_FLAG_HIDDEN);
    }

    /*
     * Wi-Fi page refresh is owned by this same UI loop.
     * The networking task still never calls LVGL.
     */
    if (current_page == PAGE_WIFI &&
        (wifi_refresh_requested ||
         (uint32_t)(now_ms - wifi_last_refresh_ms) >= 500U))
    {
        update_wifi_ui_locked();
        wifi_refresh_requested = false;
        wifi_last_refresh_ms = now_ms;
    }

    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* =========================================================
 * Main
 * ========================================================= */

void app_main(void)
{
    ESP_LOGI(TAG, "Starting KOYODA Face + Swipe Battery Page");

    /*
     * Keep the esp_lvgl_adapter render task on CPU0.
     *
     * KOYODA's shared audio owner runs on CPU1, so this keeps the
     * display/LVGL side deterministic on CPU0.
     *
     * All other Waveshare BSP defaults are preserved exactly:
     *   rotation        = 0
     *   tear avoidance  = none
     *   touch swap_xy   = 0
     *   touch mirror_x  = 1
     *   touch mirror_y  = 1
     */
    bsp_display_cfg_t display_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 1,
        },
    };

    display_cfg.lv_adapter_cfg.task_core_id = 0;

    ESP_LOGI(TAG, "Starting LVGL adapter pinned to CPU0");
    lv_display_t *koyoda_disp = bsp_display_start_with_config(&display_cfg);
    if (koyoda_disp == NULL)
    {
        ESP_LOGE(TAG, "Display/LVGL start failed");
        return;
    }

#if CONFIG_KOYODA_SMALL_DRAW_BUFFER
    /*
     * Replace the adapter's default draw buffer.
     *
     * On this board the adapter defaults to a FULL-SCREEN buffer in PSRAM
     * (466x466x2 = 434 KB). PSRAM is not DMA-capable, so every flush makes
     * spi_master allocate a temporary DMA buffer out of internal RAM. That
     * pool is also what the Opus codec and TLS need, so the two fight and
     * the loser is the display:
     *   "setup_dma_priv_buffer: Failed to allocate priv TX buffer"
     *   "Draw bitmap failed: ESP_ERR_NO_MEM"
     *
     * A small buffer that is ALREADY DMA-capable internal RAM removes the
     * temporary allocation entirely: the cost becomes a fixed, predictable
     * block instead of an unbounded per-flush request. LVGL just does more
     * smaller flushes, which this panel handles fine.
     */
    {
        const size_t lines = CONFIG_KOYODA_DRAW_BUFFER_LINES;
        const size_t buf_bytes = (size_t)466 * lines * 2; /* RGB565 */

        void *draw_buf = heap_caps_malloc(buf_bytes,
                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (draw_buf == NULL)
        {
            ESP_LOGW(TAG,
                     "Small draw buffer (%u B) unavailable; keeping adapter default",
                     (unsigned)buf_bytes);
        }
        else
        {
            bsp_display_lock(-1);
            lv_display_set_buffers(koyoda_disp, draw_buf, NULL, buf_bytes,
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
            bsp_display_unlock();
            ESP_LOGI(TAG,
                     "LVGL draw buffer: %u lines, %u B in internal DMA RAM",
                     (unsigned)lines, (unsigned)buf_bytes);
        }
    }
#endif

    if (pmu_bridge_init() != 0)
    {
        ESP_LOGE(TAG, "PMU init failed");
    }
    else
    {
        ESP_LOGI(TAG, "PMU initialized");
    }

    io_expander = bsp_io_expander_init();
    if (io_expander == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize TCA9554 IO expander");
        return;
    }

    ESP_ERROR_CHECK(
        esp_io_expander_set_dir(
            io_expander,
            IO_EXPANDER_PIN_NUM_4,
            IO_EXPANDER_INPUT));

    if (!koyoda_charge_composite_init())
    {
        ESP_LOGW(TAG, "Shared face composite buffer unavailable; charge/blink visual will stay idle");
    }

    bsp_display_lock(-1);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    face_img = lv_image_create(screen);
    lv_image_set_src(face_img, &koyoda_idle);
    lv_image_set_pivot(face_img, 233, 233);
    lv_image_set_rotation(face_img, 900);
    lv_obj_center(face_img);

    /*
     * Tiny AI overlays use the same global rotation pivot as the 466x466
     * face.  Their local pivots may be outside the cropped image bounds;
     * that is intentional: global pivot = (233,233), exactly like face_img.
     */
    thinking_overlay_img = lv_image_create(screen);
    lv_image_set_src(thinking_overlay_img, &koyoda_think_patch_01);
    lv_obj_set_pos(thinking_overlay_img,
                   KOYODA_THINK_PATCH_X,
                   KOYODA_THINK_PATCH_Y);
    lv_image_set_pivot(thinking_overlay_img,
                       233 - KOYODA_THINK_PATCH_X,
                       233 - KOYODA_THINK_PATCH_Y);
    lv_image_set_rotation(thinking_overlay_img, 900);
    lv_obj_add_flag(thinking_overlay_img, LV_OBJ_FLAG_HIDDEN);

    speaking_overlay_img = lv_image_create(screen);
    lv_image_set_src(speaking_overlay_img, &koyoda_speak_patch_closed);
    lv_obj_set_pos(speaking_overlay_img,
                   KOYODA_SPEAK_PATCH_X,
                   KOYODA_SPEAK_PATCH_Y);
    lv_image_set_pivot(speaking_overlay_img,
                       233 - KOYODA_SPEAK_PATCH_X,
                       233 - KOYODA_SPEAK_PATCH_Y);
    lv_image_set_rotation(speaking_overlay_img, 900);
    lv_obj_add_flag(speaking_overlay_img, LV_OBJ_FLAG_HIDDEN);

    /*
     * Listening notice uses a dedicated source module and a rotated logical
     * layer so it follows the same physical orientation as KOYODA's face/UI.
     */
    listening_notice_overlay =
        koyoda_listening_notice_create(screen);

    if (listening_notice_overlay == NULL)
    {
        ESP_LOGW(TAG, "Listening notice overlay creation failed");
    }

    /* Charge LITE v2 has no extra LVGL object. */

    create_battery_page(screen);
    create_wifi_page(screen);
    create_volume_page(screen);
    create_swipe_layer(screen);
    anim_reset(&animation, lv_tick_get());

    bsp_display_unlock();

    ESP_LOGI(TAG, "KOYODA UI ready: Face <-> Battery <-> Wi-Fi <-> Volume");
    ESP_LOGI(TAG, "Listening notice v5 ready: true 3-bar double-blink on AI ON + idle");

    ESP_LOGI(
        TAG,
        "AI MODE default OFF; long-press FACE for %u ms to toggle",
        (unsigned)KOYODA_AI_LONG_PRESS_MS);

    /*
     * Wi-Fi Clean Step 1:
     * start only after the known-good KOYODA UI is already visible.
     * The Wi-Fi module never touches LVGL, face_img, Battery UI, or page state.
     */
    esp_err_t wifi_err = koyoda_wifi_start();
    if (wifi_err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Wi-Fi start failed: %s; KOYODA continues offline",
            esp_err_to_name(wifi_err));
    }

    /*
     * Shared Audio Step 1:
     *
     * ONE owner initializes the BSP duplex I2S path exactly once, then opens
     * both ES7210 microphone and ES8311 speaker with the same
     * 22050 Hz / 16-bit / mono format.
     *
     * Mic capture and beep playback are serialized by that one audio task.
     * This avoids the previous Mic/Speaker init collision while keeping both
     * codec handles open and ready.
     */
    esp_err_t audio_err = koyoda_audio_duplex_start();
    if (audio_err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Shared audio start failed: %s; KOYODA continues without audio",
            esp_err_to_name(audio_err));
    }

    /*
     * Xiaozhi Backend Foundation:
     * Replaces the old raw-PCM-over-TCP link (koyoda_audio_stream.c,
     * still present in this tree for reference but no longer built into
     * app_main) with a client that speaks the xiaozhi-esp32 OTA
     * check-in + WebSocket protocol. See README-XIAOZHI-BACKEND.md.
     *
     * KOYODA stays a fully working pet with this disabled or offline:
     * it only opens a connection once Wi-Fi is up AND AI mode has been
     * turned on with the long-press gesture.
     */
    esp_err_t backend_err = koyoda_backend_start();
    if (backend_err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Xiaozhi backend start failed: %s; KOYODA continues normally",
            esp_err_to_name(backend_err));
    }

    xTaskCreate(
        power_button_task,
        "power_button",
        4096,
        NULL,
        5,
        NULL);

    xTaskCreate(
        battery_status_task,
        "battery_status",
        4096,
        NULL,
        4,
        NULL);

    /*
     * No fake boot animation here.
     * battery_status_task queues charging animation only when
     * AXP2101 reports a real charging state.
     */
    while (1)
    {
        face_animation_step();
    }
}