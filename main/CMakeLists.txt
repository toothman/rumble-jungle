if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/config.h")
    message(FATAL_ERROR
        "\nmain/config.h not found.\n"
        "Copy main/config.example.h to main/config.h and adjust it to your setup.\n")
endif()

idf_component_register(
    SRCS "main.c" "network.c" "audio.c" "dsp.c" "stream_server.c" "mqtt_ha.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s esp_eth esp_wifi esp_netif esp_event esp_timer
             esp_ringbuf nvs_flash mqtt lwip
)
