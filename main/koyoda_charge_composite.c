#include "koyoda_charge_composite.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "koyoda_ai_overlays.h"
#include "koyoda_blink_patches.h"

LV_IMAGE_DECLARE(koyoda_idle);

static const char *TAG = "KOYODA_FACE_COMP";

#define KOYODA_FACE_W 466U
#define KOYODA_FACE_H 466U
#define KOYODA_RGB565_BYTES_PER_PIXEL 2U

typedef enum
{
    COMPOSITE_KIND_NONE = 0,
    COMPOSITE_KIND_CHARGE,
    COMPOSITE_KIND_BLINK,
} composite_kind_t;

/*
 * IMPORTANT:
 * This is the SAME single full-screen PSRAM work frame used by Charge LITE v2.
 * Blink does NOT allocate a second 466x466 buffer.
 */
static uint8_t *s_face_frame = NULL;
static lv_image_dsc_t s_charge_images[4];
static lv_image_dsc_t s_blink_images[2];
static composite_kind_t s_kind = COMPOSITE_KIND_NONE;

static const size_t FACE_BYTES =
    KOYODA_FACE_W * KOYODA_FACE_H * KOYODA_RGB565_BYTES_PER_PIXEL;

static bool reset_base_if_needed(composite_kind_t requested)
{
    if (s_face_frame == NULL)
    {
        return false;
    }

    /*
     * Charge modifies the mouth region; blink modifies the eyes.
     * When switching between the two visual families, restore the full idle
     * face once so stale pixels from the previous family can never survive.
     * Consecutive frames of the same family only overwrite their compact
     * patch rectangles.
     */
    if (s_kind != requested)
    {
        memcpy(s_face_frame, koyoda_idle.data, FACE_BYTES);
        s_kind = requested;
    }

    return true;
}

static bool blit_patch(
    const lv_image_dsc_t *patch,
    unsigned x,
    unsigned y,
    unsigned expected_w,
    unsigned expected_h)
{
    if (s_face_frame == NULL || patch == NULL || patch->data == NULL)
    {
        return false;
    }

    if (patch->header.w != expected_w ||
        patch->header.h != expected_h)
    {
        ESP_LOGE(TAG,
                 "Unexpected patch size: %ux%u expected %ux%u",
                 (unsigned)patch->header.w,
                 (unsigned)patch->header.h,
                 expected_w,
                 expected_h);
        return false;
    }

    if (x + expected_w > KOYODA_FACE_W ||
        y + expected_h > KOYODA_FACE_H)
    {
        ESP_LOGE(TAG, "Patch outside face bounds");
        return false;
    }

    const size_t row_bytes =
        (size_t)expected_w * KOYODA_RGB565_BYTES_PER_PIXEL;
    const size_t expected_bytes =
        row_bytes * (size_t)expected_h;

    if (patch->data_size < expected_bytes)
    {
        ESP_LOGE(TAG,
                 "Patch data too small: %u < %u",
                 (unsigned)patch->data_size,
                 (unsigned)expected_bytes);
        return false;
    }

    for (unsigned row = 0; row < expected_h; ++row)
    {
        const size_t dst_offset =
            (((size_t)y + row) * KOYODA_FACE_W + x) *
            KOYODA_RGB565_BYTES_PER_PIXEL;
        const size_t src_offset = (size_t)row * row_bytes;

        memcpy(
            s_face_frame + dst_offset,
            patch->data + src_offset,
            row_bytes);
    }

    return true;
}

static const lv_image_dsc_t *charge_patch_for_step(unsigned step)
{
    switch (step)
    {
        case 1U:
            return &koyoda_charge_patch_taste;
        case 2U:
            return &koyoda_charge_patch_bite;
        case 3U:
            return &koyoda_charge_patch_bolt;
        case 4U:
            return &koyoda_charge_patch_glow;
        default:
            return NULL;
    }
}

bool koyoda_charge_composite_init(void)
{
    if (s_face_frame != NULL)
    {
        return true;
    }

    if (koyoda_idle.data == NULL ||
        koyoda_idle.data_size < FACE_BYTES)
    {
        ESP_LOGE(TAG,
                 "Idle image is not raw 466x466 RGB565 (data_size=%u)",
                 (unsigned)koyoda_idle.data_size);
        return false;
    }

    /*
     * Keep the proven Charge LITE v2 allocation policy:
     * external PSRAM only, never the scarce internal DMA-capable heap.
     */
    s_face_frame = (uint8_t *)heap_caps_malloc(
        FACE_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (s_face_frame == NULL)
    {
        ESP_LOGE(TAG,
                 "Failed to allocate %u-byte PSRAM shared face frame",
                 (unsigned)FACE_BYTES);
        return false;
    }

    memcpy(s_face_frame, koyoda_idle.data, FACE_BYTES);

    for (unsigned i = 0; i < 4U; ++i)
    {
        s_charge_images[i] = koyoda_idle;
        s_charge_images[i].data = s_face_frame;
        s_charge_images[i].data_size = FACE_BYTES;
    }

    for (unsigned i = 0; i < 2U; ++i)
    {
        s_blink_images[i] = koyoda_idle;
        s_blink_images[i].data = s_face_frame;
        s_blink_images[i].data_size = FACE_BYTES;
    }

    s_kind = COMPOSITE_KIND_NONE;

    ESP_LOGI(TAG,
             "Shared face composite ready in PSRAM: %u bytes (charge + blink)",
             (unsigned)FACE_BYTES);

    return true;
}

bool koyoda_charge_composite_apply(unsigned step)
{
    if (!koyoda_charge_composite_init())
    {
        return false;
    }

    const lv_image_dsc_t *patch = charge_patch_for_step(step);
    if (patch == NULL)
    {
        return false;
    }

    if (!reset_base_if_needed(COMPOSITE_KIND_CHARGE))
    {
        return false;
    }

    return blit_patch(
        patch,
        KOYODA_CHARGE_PATCH_X,
        KOYODA_CHARGE_PATCH_Y,
        KOYODA_CHARGE_PATCH_W,
        KOYODA_CHARGE_PATCH_H);
}

const lv_image_dsc_t *koyoda_charge_composite_image(unsigned step)
{
    if (s_face_frame == NULL || step < 1U || step > 4U)
    {
        return &koyoda_idle;
    }

    return &s_charge_images[step - 1U];
}

bool koyoda_blink_composite_apply(unsigned frame_id)
{
    if (!koyoda_charge_composite_init())
    {
        return false;
    }

    if (frame_id != 1U && frame_id != 2U)
    {
        return false;
    }

    if (!reset_base_if_needed(COMPOSITE_KIND_BLINK))
    {
        return false;
    }

    const lv_image_dsc_t *left =
        (frame_id == 1U)
            ? &koyoda_blink_half_left
            : &koyoda_blink_closed_left;

    const lv_image_dsc_t *right =
        (frame_id == 1U)
            ? &koyoda_blink_half_right
            : &koyoda_blink_closed_right;

    const bool left_ok = blit_patch(
        left,
        KOYODA_BLINK_LEFT_X,
        KOYODA_BLINK_LEFT_Y,
        KOYODA_BLINK_LEFT_W,
        KOYODA_BLINK_EYE_H);

    const bool right_ok = blit_patch(
        right,
        KOYODA_BLINK_RIGHT_X,
        KOYODA_BLINK_RIGHT_Y,
        KOYODA_BLINK_RIGHT_W,
        KOYODA_BLINK_EYE_H);

    return left_ok && right_ok;
}

const lv_image_dsc_t *koyoda_blink_composite_image(unsigned frame_id)
{
    if (s_face_frame == NULL || frame_id < 1U || frame_id > 2U)
    {
        return &koyoda_idle;
    }

    return &s_blink_images[frame_id - 1U];
}
