#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared KOYODA face compositor.
 *
 * Charge LITE v2 already allocates one 466x466 RGB565 work frame in PSRAM.
 * Blink LITE v4 REUSES that exact same work frame.
 *
 * Nothing here creates a separate rotated LVGL overlay object for blink or
 * charging.  The completed full-screen composite is rendered through face_img,
 * which is the path already proven stable on hardware.
 */
bool koyoda_charge_composite_init(void);

bool koyoda_charge_composite_apply(unsigned step);
const lv_image_dsc_t *koyoda_charge_composite_image(unsigned step);

bool koyoda_blink_composite_apply(unsigned frame_id);
const lv_image_dsc_t *koyoda_blink_composite_image(unsigned frame_id);

#ifdef __cplusplus
}
#endif
