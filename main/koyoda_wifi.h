#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Phone provisioning hotspot. Physical access to KOYODA is required to start it. */
#define KOYODA_WIFI_SETUP_AP_SSID      "KOYODA-Setup"
#define KOYODA_WIFI_SETUP_AP_PASSWORD  "koyoda88"

typedef enum
{
    KOYODA_WIFI_SETUP_OFF = 0,
    KOYODA_WIFI_SETUP_STARTING,
    KOYODA_WIFI_SETUP_READY,
    KOYODA_WIFI_SETUP_TESTING,
    KOYODA_WIFI_SETUP_FAILED,
    KOYODA_WIFI_SETUP_SUCCESS,
} koyoda_wifi_setup_state_t;

esp_err_t koyoda_wifi_start(void);
bool koyoda_wifi_is_connected(void);
int koyoda_wifi_get_rssi(void);
void koyoda_wifi_get_ssid(char *out, size_t out_size);

/* Starts SoftAP + captive portal without touching LVGL. */
esp_err_t koyoda_wifi_begin_provisioning(void);
bool koyoda_wifi_is_provisioning(void);
koyoda_wifi_setup_state_t koyoda_wifi_get_setup_state(void);

#ifdef __cplusplus
}
#endif
