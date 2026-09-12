#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KOYODA_BLINK_LEFT_X 58
#define KOYODA_BLINK_LEFT_Y 121
#define KOYODA_BLINK_LEFT_W 137
#define KOYODA_BLINK_EYE_H 148

#define KOYODA_BLINK_RIGHT_X 271
#define KOYODA_BLINK_RIGHT_Y 121
#define KOYODA_BLINK_RIGHT_W 138

extern const lv_image_dsc_t koyoda_blink_half_left;
extern const lv_image_dsc_t koyoda_blink_half_right;
extern const lv_image_dsc_t koyoda_blink_closed_left;
extern const lv_image_dsc_t koyoda_blink_closed_right;

#ifdef __cplusplus
}
#endif
