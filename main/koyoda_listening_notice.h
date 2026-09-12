#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t koyoda_listening_notice_bars;

/*
 * Creates the transparent rotated listening layer and returns the child image
 * object that should be shown/hidden by main.c.
 */
lv_obj_t *koyoda_listening_notice_create(lv_obj_t *screen);

#ifdef __cplusplus
}
#endif
