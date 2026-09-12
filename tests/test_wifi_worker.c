/* Execute the actual worker against controllable ESP-IDF calls. Each
 * scenario runs in its own process, just like a fresh boot. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_test_api.h"
#include "koyoda_wifi.h"
#include "koyoda_wifi_secrets.h"

const char WIFI_EVENT[] = "wifi", IP_EVENT[] = "ip";
static jmp_buf end_run;
static uint32_t clock_ms, limit_ms = 5000, connect_times[100];
static unsigned connect_count, disconnect_count, init_count, destroyed, deinitialized, unregistered;
static int fail_stage, stage, lock_depth, callback_depth, created;
static bool dropped;
static const char *scenario;
static void (*worker)(void *);
static fake_handler_t wifi_cb, ip_cb;
static void *wifi_arg, *ip_arg;
static esp_netif_t netif;

void fake_enter(portMUX_TYPE *m) { (void)m; assert(lock_depth++ == 0); }
void fake_exit(portMUX_TYPE *m) { (void)m; assert(--lock_depth == 0); }
static void driver_context(void) { assert(lock_depth == 0 && callback_depth == 0); }
static int step(void) { driver_context(); return ++stage == fail_stage ? ESP_ERR_NO_MEM : ESP_OK; }
const char *esp_err_to_name(int e) { (void)e; return "test"; }
void fake_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; assert(!lock_depth); }
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }

static void emit(esp_event_base_t base, int id, void *data)
{
    callback_depth++;
    if (base == WIFI_EVENT) wifi_cb(wifi_arg, base, id, data);
    else ip_cb(ip_arg, base, id, data);
    callback_depth--;
}
static void got_ip(void)
{
    ip_event_got_ip_t event = {.esp_netif = &netif, .ip_info.ip.a = {192,168,1,42}};
    emit(IP_EVENT, IP_EVENT_STA_GOT_IP, &event);
}
static void disconnected(int reason)
{
    wifi_event_sta_disconnected_t event = {.reason = (uint8_t)reason};
    emit(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
}

int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, int priority, void *handle)
{
    driver_context(); (void)name; (void)arg; (void)handle;
    assert(stack == 4096 && priority == 1);
    if (!strcmp(scenario,"task_fail")) return 0;
    worker = fn; created++; return pdPASS;
}
void vTaskDelete(void *task) { (void)task; assert(!lock_depth); longjmp(end_run, 2); }
void vTaskDelay(unsigned ms)
{
    assert(!lock_depth && !callback_depth);
    if (!clock_ms) assert(ms == 2000 && init_count == 0);
    else assert(ms == 250);
    clock_ms += ms;
    koyoda_wifi_status_t status;
    koyoda_wifi_get_status(&status);
    if (!dropped && clock_ms >= 35000 &&
        (!strcmp(scenario,"recover") || !strcmp(scenario,"lost_ip"))) {
        assert(status.state == KOYODA_WIFI_CONNECTED);
        dropped = true;
        if (!strcmp(scenario,"lost_ip")) emit(IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
        else disconnected(200);
    }
    if (clock_ms >= limit_ms) longjmp(end_run, 1);
}
esp_err_t nvs_flash_init(void) { return step(); }
esp_err_t esp_netif_init(void) { return step(); }
esp_err_t esp_event_loop_create_default(void) { return step(); }
esp_netif_t *esp_netif_new(const esp_netif_config_t *c) { (void)c; return step() ? NULL : &netif; }
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *n) { assert(n == &netif); return step(); }
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void) { return step(); }
void esp_netif_destroy_default_wifi(void *n) { driver_context(); assert(n == &netif); destroyed++; }
esp_err_t esp_wifi_init(const wifi_init_config_t *c) { (void)c; init_count++; return step(); }
esp_err_t esp_event_handler_instance_register(esp_event_base_t b, int32_t id, fake_handler_t cb, void *arg, esp_event_handler_instance_t *out)
{
    assert(id == ESP_EVENT_ANY_ID);
    int result = step(); if (result) return result;
    if (b == WIFI_EVENT) { wifi_cb=cb; wifi_arg=arg; *out=(void *)1; }
    else { ip_cb=cb; ip_arg=arg; *out=(void *)2; }
    return ESP_OK;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t b,int32_t id,esp_event_handler_instance_t h)
{ driver_context(); (void)b; (void)id; assert(h); unregistered++; return ESP_OK; }
esp_err_t esp_wifi_set_storage(int s) { assert(s == WIFI_STORAGE_RAM); return step(); }
esp_err_t esp_wifi_set_mode(int s) { assert(s == WIFI_MODE_STA); return step(); }
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *c)
{
    assert(interface == WIFI_IF_STA);
    assert(!memcmp(c->sta.ssid, KOYODA_WIFI_SSID, sizeof(KOYODA_WIFI_SSID)-1));
    assert(!memcmp(c->sta.password, KOYODA_WIFI_PASSWORD, sizeof(KOYODA_WIFI_PASSWORD)-1));
    return step();
}
esp_err_t esp_wifi_start(void)
{
    int result=step();
    if (!result && strcmp(scenario,"no_start")) emit(WIFI_EVENT,WIFI_EVENT_STA_START,NULL);
    return result;
}
esp_err_t esp_wifi_connect(void)
{
    driver_context(); assert(connect_count < 100);
    connect_times[connect_count++] = clock_ms;
    if (!strcmp(scenario,"connect_error")) return ESP_ERR_INVALID_STATE;
    if (!strcmp(scenario,"dhcp_timeout")) return ESP_OK;
    if (!strcmp(scenario,"missing_ap") || (!strcmp(scenario,"recover") && connect_count<3)) disconnected(201);
    else if (!strcmp(scenario,"wrong_password")) disconnected(202);
    else got_ip();
    return ESP_OK;
}
esp_err_t esp_wifi_disconnect(void)
{ driver_context(); disconnect_count++; disconnected(8); return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{ driver_context(); ap->rssi=-55; return ESP_OK; }
esp_err_t esp_wifi_stop(void) { driver_context(); return ESP_OK; }
esp_err_t esp_wifi_deinit(void) { driver_context(); deinitialized++; return ESP_OK; }

int main(int argc, char **argv)
{
    assert(argc == 2); scenario=argv[1];
    if (!strncmp(scenario,"fail_",5)) fail_stage=atoi(scenario+5);
    if (!strcmp(scenario,"task_fail")) {
        assert(koyoda_wifi_start() == ESP_ERR_NO_MEM);
        assert(created == 0 && init_count == 0); return 0;
    }
    assert(koyoda_wifi_start() == ESP_OK);
    if (sizeof(KOYODA_WIFI_SSID) == 1) {
        assert(created == 0 && !koyoda_wifi_is_connected()); return 0;
    }
    assert(koyoda_wifi_start() == ESP_OK && created == 1 && clock_ms == 0 && init_count == 0);
    if (!strcmp(scenario,"missing_ap") || !strcmp(scenario,"wrong_password") || !strcmp(scenario,"connect_error")) limit_ms=200000;
    if (!strcmp(scenario,"recover") || !strcmp(scenario,"lost_ip")) limit_ms=42000;
    if (!strcmp(scenario,"dhcp_timeout") || !strcmp(scenario,"no_start")) limit_ms=39000;
    int ended=setjmp(end_run);
    if (!ended) worker(NULL);
    koyoda_wifi_status_t s; koyoda_wifi_get_status(&s);
    if (fail_stage || !strcmp(scenario,"no_start")) {
        assert(ended == 2 && s.state == KOYODA_WIFI_ERROR);
        if (fail_stage) {
            assert(stage == fail_stage && connect_count == 0);
            assert(destroyed == (unsigned)(fail_stage > 4));
            assert(deinitialized == (unsigned)(fail_stage > 7));
            assert(unregistered == (unsigned)(fail_stage > 8) + (unsigned)(fail_stage > 9));
        }
        assert(koyoda_wifi_start() != ESP_OK && created == 1);
    } else if (!strcmp(scenario,"missing_ap") || !strcmp(scenario,"wrong_password") || !strcmp(scenario,"connect_error")) {
        assert(ended == 1 && s.state == KOYODA_WIFI_RETRYING && init_count == 1);
        assert(connect_count >= 7 && connect_count <= 10);
        for (unsigned i=1;i<connect_count;i++) {
            unsigned expected=i==1?5000:i==2?10000:i==3?20000:30000;
            assert(connect_times[i]-connect_times[i-1] >= expected);
        }
        assert(s.ip[0] == 0 && s.rssi == -127);
    } else if (!strcmp(scenario,"dhcp_timeout")) {
        assert(connect_count == 2 && disconnect_count == 1);
        assert(connect_times[1]-connect_times[0] == 35000);
    } else {
        assert(s.state == KOYODA_WIFI_CONNECTED && !strcmp(s.ip,"192.168.1.42"));
        assert(koyoda_wifi_is_connected() && koyoda_wifi_get_rssi() == -55);
        if (!strcmp(scenario,"recover") || !strcmp(scenario,"lost_ip")) {
            assert(dropped && connect_times[connect_count-1] == 40000);
            assert(connect_count == (!strcmp(scenario,"recover") ? 4U : 2U));
        } else assert(connect_count == 1);
    }
    printf("PASS worker: %s\n",scenario);
    return 0;
}
