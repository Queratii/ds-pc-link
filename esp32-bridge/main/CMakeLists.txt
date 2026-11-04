idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "../../common"
    REQUIRES esp_wifi esp_event esp_timer nvs_flash esp_driver_uart esp_ringbuf
)

# Mode MP_HOST : notre ieee80211_raw_frame_sanity_check() (main.c) doit
# remplacer celle de libnet80211 pour autoriser l'émission des trames
# d'authentification et d'association.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-zmuldefs")
