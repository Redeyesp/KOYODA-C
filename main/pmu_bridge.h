#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    bool battery_connected;
    bool vbus_in;
    bool charging;
    int battery_percent;
    uint16_t battery_voltage_mv;
} pmu_battery_status_t;

int pmu_bridge_init(void);
void pmu_bridge_shutdown(void);
int pmu_bridge_get_battery_status(pmu_battery_status_t *status);

#ifdef __cplusplus
}
#endif
