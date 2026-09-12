KOYODA Microphone Safe Probe v3
================================

Purpose
-------
Keep the ES7210 microphone genuinely active while preventing the microphone
capture loop from stretching KOYODA's blink/UI timing.

Changes from v2
---------------
- Microphone task is pinned to CPU1.
- KOYODA app_main / face animation remains on CPU0.
- Audio read block increased from 128 to 256 samples.
- Meter log reduced from every 500 ms to every 1000 ms.
- Cooperative yield after each read increased from 1 ms to 5 ms.
- No changes to face assets, blink timings, charging, Wi-Fi, Battery page,
  Tap Wake Happy, power button, or page navigation.

Expected behavior
-----------------
For the first 8 seconds KOYODA behaves normally.
Then Serial should show:
  PHASE 1/4 ...
  PHASE 2/4 ...
  PHASE 3/4 ...
  PHASE 4/4: MIC READY ...
  MIC xx% avg=... peak=...

Blink should remain at the existing ~3 second idle cadence after MIC READY.

Upload/replace
--------------
main/koyoda_mic_probe.c

main.c and CMakeLists.txt from v2 do not need another change.
