idf_component_register(
    SRCS "main.c" "settings.c" "network.c" "audio.c" "mixer.c" "dsp.c"
         "stream_server.c" "web_server.c" "mqtt_ha.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s esp_eth esp_netif esp_event esp_timer esp_ringbuf
             nvs_flash mqtt lwip esp_http_server json app_update esp_app_format
    EMBED_FILES "www/index.html.gz" "www/app.js.gz" "www/app.css.gz"
)
