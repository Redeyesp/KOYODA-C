#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    KOYODA_FACE_AI_IDLE = 0,
    KOYODA_FACE_AI_THINKING,
    KOYODA_FACE_AI_SPEAKING,
} koyoda_face_ai_state_t;

/*
 * Thread-safe state bridge only. These functions NEVER call LVGL.
 * The UI owner in main.c reads the state and changes face_img under the
 * existing BSP display lock.
 */
void koyoda_face_state_set(koyoda_face_ai_state_t state);
koyoda_face_ai_state_t koyoda_face_state_get(void);

#ifdef __cplusplus
}
#endif
