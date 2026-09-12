#include <cstring>

#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"

#include "bsp/esp-bsp.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include "pmu_bridge.h"

static const char *TAG = "PMU_BRIDGE";

static XPowersPMU PMU;
static i2c_master_dev_handle_t pmu_dev_handle = NULL;
static bool pmu_initialized = false;


static int pmu_register_read(
    uint8_t devAddr,
    uint8_t regAddr,
    uint8_t *data,
    uint8_t len
)
{
    (void)devAddr;

    esp_err_t ret = i2c_master_transmit_receive(
        pmu_dev_handle,
        &regAddr,
        1,
        data,
        len,
        1000
    );

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "PMU read failed: %s", esp_err_to_name(ret));
        return -1;
    }

    return 0;
}


static int pmu_register_write_byte(
    uint8_t devAddr,
    uint8_t regAddr,
    uint8_t *data,
    uint8_t len
)
{
    (void)devAddr;

    uint8_t buffer[32];

    if ((len + 1) > sizeof(buffer))
    {
        ESP_LOGE(TAG, "PMU write too large");
        return -1;
    }

    buffer[0] = regAddr;
    memcpy(&buffer[1], data, len);

    esp_err_t ret = i2c_master_transmit(
        pmu_dev_handle,
        buffer,
        len + 1,
        1000
    );

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "PMU write failed: %s", esp_err_to_name(ret));
        return -1;
    }

    return 0;
}


extern "C" int pmu_bridge_init(void)
{
    if (pmu_initialized)
    {
        return 0;
    }

    ESP_LOGI(TAG, "Initializing AXP2101");

    i2c_master_bus_handle_t bus =
        bsp_i2c_get_handle();

    if (bus == NULL)
    {
        ESP_LOGE(TAG, "Failed to get BSP I2C bus");
        return -1;
    }


    i2c_device_config_t dev_cfg = {};

    dev_cfg.dev_addr_length =
        I2C_ADDR_BIT_LEN_7;

    dev_cfg.device_address =
        AXP2101_SLAVE_ADDRESS;

    dev_cfg.scl_speed_hz =
        400000;


    esp_err_t ret =
        i2c_master_bus_add_device(
            bus,
            &dev_cfg,
            &pmu_dev_handle
        );


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to add AXP2101 device: %s",
            esp_err_to_name(ret)
        );

        return -1;
    }


    if (!PMU.begin(
            AXP2101_SLAVE_ADDRESS,
            pmu_register_read,
            pmu_register_write_byte))
    {
        ESP_LOGE(
            TAG,
            "PMU.begin failed"
        );

        return -1;
    }

    /* Battery status page needs these measurements enabled. */
    PMU.enableBattDetection();
    PMU.enableBattVoltageMeasure();

    pmu_initialized = true;

    ESP_LOGI(
        TAG,
        "AXP2101 ready"
    );

    return 0;
}


extern "C" void pmu_bridge_shutdown(void)
{
    if (!pmu_initialized)
    {
        ESP_LOGW(TAG, "Shutdown requested before PMU initialization");
        return;
    }

    ESP_LOGI(
        TAG,
        "AXP2101 shutdown requested"
    );

    PMU.shutdown();
}


extern "C" int pmu_bridge_get_battery_status(pmu_battery_status_t *status)
{
    if (!pmu_initialized || status == NULL)
    {
        return -1;
    }

    status->battery_connected = PMU.isBatteryConnect();
    status->vbus_in = PMU.isVbusIn();
    status->charging = PMU.isCharging();

    if (status->battery_connected)
    {
        status->battery_percent = PMU.getBatteryPercent();
        status->battery_voltage_mv = PMU.getBattVoltage();
    }
    else
    {
        status->battery_percent = -1;
        status->battery_voltage_mv = 0;
    }

    return 0;
}
