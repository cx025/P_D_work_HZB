idf_component_register(SRCS
                            "pressure_sensor.c"
                            "pressure_test.c"
                            "drv8833.c"
                            "speaker.c"
                            "speaker_mp3.cpp"
                            "ble_control.c"
                            "record_log.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "fart_soft.mp3"
                                   "fart_blast.mp3"
                                   "fart_awkward.mp3"
                       REQUIRES bt esp_driver_gpio esp_driver_i2s esp_driver_ledc micro-mp3
                                esp_timer nvs_flash littlefs
                       PRIV_REQUIRES esp_pm)
