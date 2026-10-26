idf_component_register(SRCS "main.c" "audio.c" "gemini.c" "wake.c" "settings.c" "web.c" "ha.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi nvs_flash esp_driver_i2s esp_driver_i2c esp_driver_gpio json mbedtls
                                esp_timer esp_http_server esp_http_client esp_netif esp_app_format app_update bootloader_support
                       EMBED_TXTFILES "web/index.html"
                       EMBED_FILES "clips/quota.pcm" "clips/apikey.pcm" "clips/wifi.pcm" "clips/connect.pcm")
