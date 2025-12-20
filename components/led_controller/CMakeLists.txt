idf_component_register(SRCS "led_controller.c" "led_config_nvs.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_timer freertos nvs_flash)
