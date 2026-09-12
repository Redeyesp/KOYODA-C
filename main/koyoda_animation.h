#pragma once
#include <stdbool.h>
#include <stdint.h>

#define KOYODA_SLEEP_AFTER_MS (5U * 60U * 1000U)

typedef enum
{
    ANIM_IDLE,
    ANIM_DROWSY,
    ANIM_SLEEP,
    ANIM_CHARGE,
    ANIM_WAKE
} anim_mode_t;

typedef struct
{
    anim_mode_t mode;
    unsigned step;
    uint32_t frame_started, last_activity;
} koyoda_animation_t;

/*
 * Frame IDs used by main.c:
 * 0  = idle
 * 1  = half
 * 2  = closed
 * 3..5  = sleep 1..3
 *
 * Charging no longer needs full-screen frame IDs. ANIM_CHARGE keeps
 * the base face on idle while main.c uses the shared PSRAM compositor.
 */
static const unsigned anim_idle_frames[]   = {0, 1, 2, 1};
static const uint16_t anim_idle_ms[]       = {3000, 60, 90, 60};

static const unsigned anim_drowsy_frames[] = {0, 1, 2, 2};
static const uint16_t anim_drowsy_ms[]     = {600, 800, 500, 500};

static const unsigned anim_sleep_frames[]  = {3, 4, 5, 4, 3};
static const uint16_t anim_sleep_ms[]      = {800, 800, 1000, 800, 800};

/*
 * One-shot "eat electricity" sequence on VBUS insertion:
 * idle -> taste -> bite -> lightning gulp -> glow/swallow -> idle
 *
 * All frame IDs stay 0 because koyoda_idle is the base image throughout.
 * main.c chooses the compact overlay from animation.step.
 */
static const unsigned anim_charge_frames[] = {0, 0, 0, 0, 0, 0};
static const uint16_t anim_charge_ms[]     = {180, 160, 180, 260, 220, 350};

/*
 * Tap-to-wake sequence requested:
 * Closed -> Half Closed -> Idle -> Idle + open-mouth overlay -> Idle
 *
 * The final wake expression reuses KOYODA's existing speak-open mouth patch.
 * This removes the separate full-screen fun_happy image from firmware.
 */
static const unsigned anim_wake_frames[]   = {2, 1, 0, 0};
static const uint16_t anim_wake_ms[]       = {450, 500, 650, 1100};

static inline void anim_reset(koyoda_animation_t *a, uint32_t now)
{
    *a = (koyoda_animation_t){ANIM_IDLE, 0, now, now};
}

static inline bool anim_touch(koyoda_animation_t *a, uint32_t now)
{
    bool wake =
        a->mode == ANIM_DROWSY ||
        a->mode == ANIM_SLEEP;

    a->last_activity = now;

    if (wake)
    {
        a->mode = ANIM_WAKE;
        a->step = 0;
        a->frame_started = now;
    }

    return wake;
}

static inline unsigned anim_tick(
    koyoda_animation_t *a,
    uint32_t now,
    bool visible,
    bool blocked,
    bool held,
    bool *pending)
{
    if (!visible || blocked)
    {
        if (a->mode == ANIM_CHARGE)
        {
            *pending = true;
        }

        anim_reset(a, now);
        return 0;
    }

    if (held)
    {
        a->last_activity = now;
    }

    /*
     * Charging remains higher priority than normal idle/sleep,
     * but a wake transition is allowed to finish first.
     */
    if (*pending &&
        a->mode != ANIM_CHARGE &&
        a->mode != ANIM_WAKE)
    {
        *pending = false;
        a->mode = ANIM_CHARGE;
        a->step = 0;
        a->frame_started = now;
        a->last_activity = now;
    }

    if (a->mode == ANIM_IDLE &&
        !held &&
        (uint32_t)(now - a->last_activity) >= KOYODA_SLEEP_AFTER_MS)
    {
        a->mode = ANIM_DROWSY;
        a->step = 0;
        a->frame_started = now;
    }

    const unsigned *frames = anim_idle_frames;
    const uint16_t *delays = anim_idle_ms;
    unsigned count = 4;

    switch (a->mode)
    {
        case ANIM_DROWSY:
            frames = anim_drowsy_frames;
            delays = anim_drowsy_ms;
            break;

        case ANIM_SLEEP:
            frames = anim_sleep_frames;
            delays = anim_sleep_ms;
            count = 5;
            break;

        case ANIM_CHARGE:
            frames = anim_charge_frames;
            delays = anim_charge_ms;
            count = 6;
            break;

        case ANIM_WAKE:
            frames = anim_wake_frames;
            delays = anim_wake_ms;
            count = 4;
            break;

        default:
            break;
    }

    if ((uint32_t)(now - a->frame_started) >= delays[a->step])
    {
        a->frame_started = now;

        if (++a->step == count)
        {
            a->step = 0;

            if (a->mode == ANIM_DROWSY)
            {
                a->mode = ANIM_SLEEP;
                return 3;
            }

            if (a->mode == ANIM_CHARGE)
            {
                anim_reset(a, now);
                return 0;
            }

            if (a->mode == ANIM_WAKE)
            {
                anim_reset(a, now);
                return 0;
            }
        }
    }

    return frames[a->step];
}
