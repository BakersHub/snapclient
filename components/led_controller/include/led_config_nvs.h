#ifndef LED_CONFIG_NVS_H
#define LED_CONFIG_NVS_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// LED configuration structure for NVS storage
typedef struct {
    uint16_t num_leds;
    uint8_t gpio_pin;
    uint8_t enabled;
    uint8_t color_order;  // 0 = RGB, 1 = GRB
    uint8_t brightness;
    uint8_t effect;
    uint8_t speed;
    uint8_t sensitivity;
    uint8_t color_r;
    uint8_t color_g;
    uint8_t color_b;
    uint8_t color2_r;
    uint8_t color2_g;
    uint8_t color2_b;
    uint8_t ignore_volume;   // 0 = follow playback volume, 1 = auto-normalize input
    uint8_t bass_focus;      // 0 = full-spectrum, 255 = strongly bass-focused
    uint8_t auto_rainbow;    // 0 = use static color1, 1 = cycle color1 through rainbow
    uint8_t rainbow_speed;   // 0-255: speed of auto-rainbow hue animation for color1
    uint8_t auto_rainbow2;   // 0 = use static color2, 1 = cycle color2 through rainbow
    uint8_t rainbow_speed2;  // 0-255: speed of auto-rainbow hue animation for color2
} led_config_nvs_t;

/**
 * @brief Save LED configuration to NVS
 */
esp_err_t led_config_save_to_nvs(const led_config_nvs_t *config);

/**
 * @brief Load LED configuration from NVS
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if no config saved
 */
esp_err_t led_config_load_from_nvs(led_config_nvs_t *config);

/**
 * @brief Get default LED configuration from Kconfig
 */
void led_config_get_defaults(led_config_nvs_t *config);

#ifdef __cplusplus
}
#endif

#endif // LED_CONFIG_NVS_H
