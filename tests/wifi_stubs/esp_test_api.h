#pragma once
/* Host fakes for control-flow tests, not an ESP-IDF SDK or hardware model. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
const char *esp_err_to_name(esp_err_t);
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void fake_enter(portMUX_TYPE *);
void fake_exit(portMUX_TYPE *);
#define portENTER_CRITICAL(m) fake_enter(m)
#define portEXIT_CRITICAL(m) fake_exit(m)
typedef int BaseType_t;
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, int, void *);
void vTaskDelete(void *);
void vTaskDelay(unsigned);
int64_t esp_timer_get_time(void);
void fake_log(const char *, const char *, ...);
#define ESP_LOGI(tag, ...) fake_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) fake_log(tag, __VA_ARGS__)
typedef const char *esp_event_base_t;
extern const char WIFI_EVENT[], IP_EVENT[];
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define WIFI_EVENT_STA_STOP 3
#define IP_EVENT_STA_GOT_IP 4
#define IP_EVENT_STA_LOST_IP 5
typedef void *esp_event_handler_instance_t;
typedef void (*fake_handler_t)(void *, esp_event_base_t, int32_t, void *);
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t, int32_t, fake_handler_t, void *, esp_event_handler_instance_t *);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t, int32_t, esp_event_handler_instance_t);
typedef struct { unsigned value; } esp_netif_t;
typedef struct { unsigned value; } esp_netif_config_t;
#define ESP_NETIF_DEFAULT_WIFI_STA() (esp_netif_config_t){0}
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_new(const esp_netif_config_t *);
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *);
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void);
void esp_netif_destroy_default_wifi(void *);
typedef struct { uint8_t a[4]; } fake_ip_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) (ip)->a[0],(ip)->a[1],(ip)->a[2],(ip)->a[3]
typedef struct { esp_netif_t *esp_netif; struct { fake_ip_t ip; } ip_info; } ip_event_got_ip_t;
typedef struct { uint8_t reason; } wifi_event_sta_disconnected_t;
typedef struct { unsigned value; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() (wifi_init_config_t){0}
typedef struct { struct {
    uint8_t ssid[32], password[64];
    struct { int authmode; } threshold;
    struct { bool capable, required; } pmf_cfg;
} sta; } wifi_config_t;
typedef struct { int8_t rssi; } wifi_ap_record_t;
#define WIFI_STORAGE_RAM 0
#define WIFI_MODE_STA 1
#define WIFI_IF_STA 0
#define WIFI_AUTH_WPA2_PSK 3
esp_err_t nvs_flash_init(void);
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_set_config(int, const wifi_config_t *);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_deinit(void);
