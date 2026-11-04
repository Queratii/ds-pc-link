idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "../../common"
    REQUIRES esp_wifi esp_event esp_timer nvs_flash esp_driver_uart esp_ringbuf
)
