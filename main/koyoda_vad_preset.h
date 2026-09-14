#pragma once

/*
 * KOYODA VAD presets
 * ==================
 *
 * These four numbers decide when KOYODA thinks you have started and
 * finished speaking. They live here rather than in koyoda_audio_duplex.c
 * so that changing a preset does not mean editing the audio pipeline.
 *
 * Selected at build time via sdkconfig.defaults:
 *   CONFIG_KOYODA_VAD_SENSITIVE / _NORMAL / _OUTDOOR
 *
 * NORMAL reproduces the original hard-coded values exactly, so the
 * default build behaves identically to every version tested so far.
 *
 * How the threshold works:
 *     threshold = max(MIN_START_LEVEL, noise_floor * MULTIPLIER + MARGIN)
 * The noise floor adapts on its own while idle, so these presets change
 * how *aggressive* KOYODA is relative to the room, not a fixed level.
 * Measured noise floors: quiet room ~10, home ~20, office ~50.
 *
 * Why END_SILENCE_MS matters most: at 850 ms a natural mid-sentence pause
 * ends the utterance, and the server then transcribes half a sentence.
 * This was visible in real logs, e.g. "I ordered something called a."
 * followed separately by "lamati." -- one sentence, cut in two.
 */

#if defined(CONFIG_KOYODA_VAD_SENSITIVE)

/* Quiet room, soft or slow speech. Picks up more, including some fan and
 * air-conditioning noise. */
#define KOYODA_VAD_PRESET_NAME              "SENSITIVE"
#define VAD_START_CONSECUTIVE_FRAMES         2
#define VAD_END_SILENCE_MS                1200
#define VAD_MIN_START_LEVEL                  50U
#define VAD_NOISE_MULTIPLIER                  2U
#define VAD_NOISE_MARGIN                     20U

#elif defined(CONFIG_KOYODA_VAD_OUTDOOR)

/* Crowds, traffic, a noisy cafe. Needs deliberate speech aimed at the
 * device and ends utterances promptly so background chatter cannot keep
 * a session open. */
#define KOYODA_VAD_PRESET_NAME              "OUTDOOR"
#define VAD_START_CONSECUTIVE_FRAMES         4
#define VAD_END_SILENCE_MS                 700
#define VAD_MIN_START_LEVEL                 180U
#define VAD_NOISE_MULTIPLIER                  5U
#define VAD_NOISE_MARGIN                     80U

#else /* CONFIG_KOYODA_VAD_NORMAL, and the fallback when Kconfig is absent */

/* EXACTLY the original values. Do not change these: this is the baseline
 * every earlier test was measured against. */
#define KOYODA_VAD_PRESET_NAME              "NORMAL"
#define VAD_START_CONSECUTIVE_FRAMES         3
#define VAD_END_SILENCE_MS                 850
#define VAD_MIN_START_LEVEL                  70U
#define VAD_NOISE_MULTIPLIER                  3U
#define VAD_NOISE_MARGIN                     30U

#endif
