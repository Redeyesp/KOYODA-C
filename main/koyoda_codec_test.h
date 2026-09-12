#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KOYODA codec self-test
 * ======================
 *
 * Verifies the Opus chain WITHOUT any server, Wi-Fi or microphone:
 *
 *   generated 440 Hz tone @22050
 *     -> resample 16000 -> Opus encode -> Opus decode -> resample 22050
 *   -> measured
 *
 * It checks the three things most likely to be wrong, in the order they
 * are most likely to be wrong:
 *
 *   1. SAMPLE COUNT. Output length should match input length within a
 *      few percent. If the resamplers are misconfigured the length comes
 *      back scaled by 16000/22050 (0.73x) or 22050/16000 (1.38x).
 *
 *   2. PITCH. A 440 Hz tone in must be ~440 Hz out. This is the decisive
 *      check: if one resampling stage is missing or reversed the tone
 *      comes back at ~319 Hz or ~606 Hz while the sample count can still
 *      look plausible. Measured by zero-crossing rate, which is cheap and
 *      unambiguous for a pure tone.
 *
 *   3. LEVEL. Output RMS must be in the same ballpark as input. Catches
 *      silence, near-silence and gross clipping.
 *
 * Results are printed to the serial log as PASS/FAIL with the actual
 * numbers, so a failure tells you which stage is wrong rather than just
 * "it did not work".
 */
esp_err_t koyoda_codec_selftest_run(void);

#ifdef __cplusplus
}
#endif
