#include "led_config_nvs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "LED_CONFIG";
static const char *NVS_NAMESPACE = "led_config";

esp_err_t led_config_save_to_nvs(const led_config_nvs_t *config) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    // Save LED configuration
    nvs_set_u16(nvs_handle, "num_leds", config->num_leds);
    nvs_set_u8(nvs_handle, "gpio_pin", config->gpio_pin);
    nvs_set_u8(nvs_handle, "enabled", config->enabled);
    nvs_set_u8(nvs_handle, "color_order", config->color_order);
    nvs_set_u8(nvs_handle, "brightness", config->brightness);
    nvs_set_u8(nvs_handle, "effect", config->effect);
    nvs_set_u8(nvs_handle, "speed", config->speed);
    nvs_set_u8(nvs_handle, "sensitivity", config->sensitivity);
    nvs_set_u8(nvs_handle, "color_r", config->color_r);
    nvs_set_u8(nvs_handle, "color_g", config->color_g);
    nvs_set_u8(nvs_handle, "color_b", config->color_b);
    nvs_set_u8(nvs_handle, "color2_r", config->color2_r);
    nvs_set_u8(nvs_handle, "color2_g", config->color2_g);
    nvs_set_u8(nvs_handle, "color2_b", config->color2_b);
    nvs_set_u8(nvs_handle, "ignore_volume", config->ignore_volume);
    nvs_set_u8(nvs_handle, "bass_focus", config->bass_focus);
    nvs_set_u8(nvs_handle, "auto_rainbow", config->auto_rainbow);
    nvs_set_u8(nvs_handle, "rainbow_speed", config->rainbow_speed);
    nvs_set_u8(nvs_handle, "auto_rainbow2", config->auto_rainbow2);
    nvs_set_u8(nvs_handle, "rainbow_speed2", config->rainbow_speed2);

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "LED config saved to NVS");
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t led_config_load_from_nvs(led_config_nvs_t *config) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Start with defaults so any missing keys keep a sane value
    led_config_get_defaults(config);

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved config found, using defaults");
        return ESP_ERR_NOT_FOUND;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    // Load LED configuration with defaults from Kconfig
    nvs_get_u16(nvs_handle, "num_leds", &config->num_leds);
    nvs_get_u8(nvs_handle, "gpio_pin", &config->gpio_pin);
    nvs_get_u8(nvs_handle, "enabled", &config->enabled);
    nvs_get_u8(nvs_handle, "color_order", &config->color_order);
    nvs_get_u8(nvs_handle, "brightness", &config->brightness);
    nvs_get_u8(nvs_handle, "effect", &config->effect);
    nvs_get_u8(nvs_handle, "speed", &config->speed);
    nvs_get_u8(nvs_handle, "sensitivity", &config->sensitivity);
    nvs_get_u8(nvs_handle, "color_r", &config->color_r);
    nvs_get_u8(nvs_handle, "color_g", &config->color_g);
    nvs_get_u8(nvs_handle, "color_b", &config->color_b);
    nvs_get_u8(nvs_handle, "color2_r", &config->color2_r);
    nvs_get_u8(nvs_handle, "color2_g", &config->color2_g);
    nvs_get_u8(nvs_handle, "color2_b", &config->color2_b);
    nvs_get_u8(nvs_handle, "ignore_volume", &config->ignore_volume);
    nvs_get_u8(nvs_handle, "bass_focus", &config->bass_focus);
    nvs_get_u8(nvs_handle, "auto_rainbow", &config->auto_rainbow);
    nvs_get_u8(nvs_handle, "rainbow_speed", &config->rainbow_speed);
    nvs_get_u8(nvs_handle, "auto_rainbow2", &config->auto_rainbow2);
    nvs_get_u8(nvs_handle, "rainbow_speed2", &config->rainbow_speed2);

    ESP_LOGI(TAG, "LED config loaded from NVS: %d LEDs, GPIO %d", 
             config->num_leds, config->gpio_pin);

    nvs_close(nvs_handle);
    return ESP_OK;
}

void led_config_get_defaults(led_config_nvs_t *config) {
    config->num_leds = CONFIG_LED_NUM_LEDS;
    config->gpio_pin = CONFIG_LED_GPIO_PIN;
    config->enabled = 1;
    config->color_order = 0; // default RGB
    config->brightness = CONFIG_LED_DEFAULT_BRIGHTNESS;
    config->effect = CONFIG_LED_DEFAULT_EFFECT;
    config->speed = 128;
    config->sensitivity = CONFIG_LED_DEFAULT_SENSITIVITY;
    config->color_r = 255;
    config->color_g = 0;
    config->color_b = 0;
     // Secondary default: cool blue
    config->color2_r = 0;
    config->color2_g = 0;
    config->color2_b = 255;
    config->ignore_volume = 0; // default: follow playback volume
    config->bass_focus = 128;  // default: balanced between full and bass-focused
    config->auto_rainbow = 0;   // default: use static primary color
    config->rainbow_speed = 128; // mid speed by default for color1
    config->auto_rainbow2 = 0;  // default: use static secondary color
    config->rainbow_speed2 = 128; // mid speed by default for color2
}
