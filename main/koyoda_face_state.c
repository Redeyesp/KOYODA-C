#include "koyoda_face_state.h"

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

static const char *TAG = "KOYODA_FACE";
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static koyoda_face_ai_state_t s_state = KOYODA_FACE_AI_IDLE;

static const char *state_name(koyoda_face_ai_state_t state)
{
    switch (state)
    {
        case KOYODA_FACE_AI_THINKING: return "THINKING";
        case KOYODA_FACE_AI_SPEAKING: return "SPEAKING";
        default: return "IDLE";
    }
}

void koyoda_face_state_set(koyoda_face_ai_state_t state)
{
    if (state < KOYODA_FACE_AI_IDLE || state > KOYODA_FACE_AI_SPEAKING)
    {
        state = KOYODA_FACE_AI_IDLE;
    }

    bool changed = false;

    portENTER_CRITICAL(&s_state_lock);
    if (s_state != state)
    {
        s_state = state;
        changed = true;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (changed)
    {
        ESP_LOGI(TAG, "AI FACE -> %s", state_name(state));
    }
}

koyoda_face_ai_state_t koyoda_face_state_get(void)
{
    koyoda_face_ai_state_t state;

    portENTER_CRITICAL(&s_state_lock);
    state = s_state;
    portEXIT_CRITICAL(&s_state_lock);

    return state;
}
