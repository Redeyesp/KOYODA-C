#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KOYODA VAD presets (runtime selectable)
 * =======================================
 *
 * These five numbers decide when KOYODA thinks you have started and
 * finished speaking. They are deliberately NOT microphone gain: raising
 * gain lifts speech and room noise equally and was measured to make
 * recognition worse. These change how KOYODA judges speech relative to
 * the room's own noise level, which is what actually differs between a
 * bedroom, an office and a street.
 *
 * The threshold is:
 *     max(min_start_level, noise_floor * noise_multiplier + noise_margin)
 * and the noise floor already adapts on its own while idle (about 20 at
 * home, about 50 in an office).
 *
 * end_silence_ms matters most in practice: at 850 ms a natural pause in
 * the middle of a sentence ends the utterance and the server transcribes
 * only half of it. Real logs showed "I ordered something called a."
 * followed separately by "lamati." -- one sentence, cut in two.
 *
 * HOW THIS STAYS OUT OF THE AUDIO PIPELINE
 * koyoda_audio_duplex.c keeps using the same macro names it always did;
 * they simply resolve to fields of the struct below instead of literals.
 * That file therefore needs no change beyond including this header, and
 * NORMAL reproduces its original constants exactly.
 *
 * THREADING
 * The audio task reads these fields; the UI task writes them. Each field
 * is a naturally aligned 32-bit word, so a read never sees a torn value.
 * A change can land between two frames of one utterance, which at worst
 * shifts a single VAD decision -- harmless, and not worth adding a lock
 * to the audio path.
 */

/*
 * Named after the SITUATION the user is in, not after how sensitive the
 * detector is. "Normal" stopped being meaningful once QUIET became the
 * default, and a person can tell you which room they are in far more
 * reliably than which sensitivity they want.
 *
 * The numeric order is deliberately unchanged (0/1/2) so a device that
 * already stored a choice in NVS keeps the same settings after this
 * rename.
 */
typedef enum
{
    KOYODA_VAD_PRESET_QUIET = 0,   /* was SENSITIVE */
    KOYODA_VAD_PRESET_BUSY,        /* was NORMAL    */
    KOYODA_VAD_PRESET_OUTDOOR,
    KOYODA_VAD_PRESET_COUNT
} koyoda_vad_preset_t;

typedef struct
{
    uint32_t start_frames;
    uint32_t end_silence_ms;
    uint32_t min_start_level;
    uint32_t noise_multiplier;
    uint32_t noise_margin;
} koyoda_vad_params_t;

extern koyoda_vad_params_t g_koyoda_vad;

/* The names koyoda_audio_duplex.c has always used. */
#define VAD_START_CONSECUTIVE_FRAMES  (g_koyoda_vad.start_frames)
#define VAD_END_SILENCE_MS            (g_koyoda_vad.end_silence_ms)
#define VAD_MIN_START_LEVEL           (g_koyoda_vad.min_start_level)
#define VAD_NOISE_MULTIPLIER          (g_koyoda_vad.noise_multiplier)
#define VAD_NOISE_MARGIN              (g_koyoda_vad.noise_margin)

/* Load the saved preset from NVS, or fall back to NORMAL. Call once from
 * app_main before the audio task starts. */
void koyoda_vad_preset_init(void);

koyoda_vad_preset_t koyoda_vad_preset_get(void);

/* Apply a preset and persist it. Safe to call from the UI task. */
void koyoda_vad_preset_set(koyoda_vad_preset_t preset);

/* "QUIET" / "BUSY" / "OUTDOOR" */
const char *koyoda_vad_preset_name(koyoda_vad_preset_t preset);

/* One short line for the UI, e.g. "Quiet room, soft speech". */
const char *koyoda_vad_preset_hint(koyoda_vad_preset_t preset);

#ifdef __cplusplus
}
#endif
