idf_component_register(SRCS "main.c" "motion_detect.c" "event_queue.c" "state_machine.c" "config.c" "event_log.c"
                    "wifi_manager.c" "mqtt_manager.c" "voice.c" "voice_player.c"
                    INCLUDE_DIRS "."
                    REQUIRES BSP esp_wifi nvs_flash mqtt esp_netif esp_event esp_timer esp_codec_dev esp_driver_i2s
                    EMBED_FILES "assets/welcome.pcm" "assets/sleepy.pcm")