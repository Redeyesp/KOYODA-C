idf_component_register(
    SRCS "main.c"
         "pmu_bridge.cpp"
         "koyoda_idle.c"
         "koyoda_sleep_1.c"
         "koyoda_sleep_2.c"
         "koyoda_sleep_3.c"
         "koyoda_wifi.c"
         "koyoda_audio_duplex.c"
         "koyoda_backend.c"
         "koyoda_codec.c"
         "koyoda_codec_test.c"
         "koyoda_face_state.c"
         "koyoda_ai_overlays.c"
         "koyoda_charge_composite.c"
         "koyoda_blink_patches.c"
         "koyoda_listening_notice.c"
    # koyoda_audio_stream.c (the old raw-PCM-over-TCP link to
    # tools/koyoda_pcm_receiver.py) is intentionally NOT built anymore.
    # It's kept in this folder for reference; koyoda_backend.c replaces
    # it with the xiaozhi-esp32 OTA + WebSocket protocol.
    INCLUDE_DIRS "."
    REQUIRES
         XPowersLib
         esp_wifi
         esp_event
         esp_netif
         nvs_flash
         esp_http_server
         esp_http_client
         esp_https_ota
         app_update
         mbedtls
         json
         # Needed by the OTA check-in body: esp_psram.h reports PSRAM size,
         # esp_flash.h reports flash size. Both are separate components in
         # ESP-IDF 5.x and are not pulled in transitively.
         esp_psram
         spi_flash
         esp_partition
    # NOTE: esp_websocket_client is intentionally NOT listed here. It is a
    # managed component declared in idf_component.yml, and the component
    # manager wires it into this component automatically.
)
