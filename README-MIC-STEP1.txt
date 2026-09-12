KOYODA Microphone Probe — Step 1
================================

BASE
----
Prepared for the current KOYODA Wi-Fi Status baseline.

GOAL
----
Prove the two onboard microphones work before we add AI, STT, VAD,
wake word, speaker playback, or mouth animation.

HARDWARE PATH
-------------
Two onboard microphones
  -> ES7210 audio ADC
  -> ESP32-S3 I2S RX / four-slot TDM

Waveshare's maintained voice profile is 24 kHz, 16-bit.
The four RX slots are:
  slot 0 = MIC1
  slot 1 = playback reference
  slot 2 = MIC2
  slot 3 = unused

WHAT THIS PATCH DOES
--------------------
- Starts the microphone about 3 seconds after boot.
- Uses the BSP's 24 kHz voice/TDM initialization.
- Reads MIC1 and MIC2 continuously.
- Prints a simple diagnostic level + average + peak to Serial every 300 ms.
- Adds NO microphone page yet.
- Adds NO mouth movement yet.
- Sends NO audio over Wi-Fi yet.
- Does NOT touch LVGL, face_img, Battery page, Wi-Fi page, charging,
  blink, sleep, tap wake, swipe, or gyro.

FILES TO REPLACE / ADD
----------------------
REPLACE:
  main/main.c
  main/CMakeLists.txt

ADD:
  main/koyoda_mic_probe.c
  main/koyoda_mic_probe.h

EXPECTED SERIAL
---------------
KOYODA_MIC: Mic probe scheduled; UI/animation untouched
KOYODA_MIC: Initializing ES7210 microphone probe
KOYODA_MIC: MIC READY: ES7210 24kHz 16-bit 4-slot TDM; monitoring MIC1 + MIC2
KOYODA_MIC: MIC1  12% avg= 1450 peak= 6220 | MIC2  10% avg= 1210 peak= 5900

Speak or tap near each microphone.  The numbers should rise.

IMPORTANT
---------
The displayed percentage is only a convenient raw-audio activity meter,
NOT a calibrated sound-pressure measurement.

If the display, blink, charging, Wi-Fi status, or swipe changes after this
patch, stop there and return to the stable-wifi-status tag.  Do not layer
additional AI code on top until the audio probe is stable.
