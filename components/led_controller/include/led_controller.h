#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// LED effect types
typedef enum {
    LED_EFFECT_OFF = 0,
    LED_EFFECT_VU_METER,
    LED_EFFECT_SPECTRUM_ANALYZER,
    LED_EFFECT_PULSE,
    LED_EFFECT_WAVE,
    LED_EFFECT_ENERGY_BAR,
    LED_EFFECT_RAINBOW_PULSE,
    LED_EFFECT_BEAT_FLASH,
    LED_EFFECT_BASS_PULSE,
    LED_EFFECT_STEREO_VU,
    LED_EFFECT_SOLID_COLOR,
    LED_EFFECT_COLOR_WIPE,
    LED_EFFECT_DUAL_COLOR_WAVE,
    LED_EFFECT_COMET,
    LED_EFFECT_SPARKLE,
    LED_EFFECT_BEAT_RAINBOW,
    LED_EFFECT_MULTI_COMET,
    LED_EFFECT_MARCHING,
    LED_EFFECT_BLADE_POWER,
    LED_EFFECT_SCAN_MULTI,
    LED_EFFECT_PITCH_SPECTRUM,
    LED_EFFECT_EQUALIZER,
    LED_EFFECT_DUAL_ENERGY_BAR,
    LED_EFFECT_CHARGE_BEAM,
    LED_EFFECT_ENERGY_COMET,
    LED_EFFECT_SEGMENT_LEVELS,
    LED_EFFECT_CENTER_PULSE,
    LED_EFFECT_RAINBOW_PLASMA,
    LED_EFFECT_NOISE_PLASMA,
    LED_EFFECT_MAX
} led_effect_t;

// LED color structure (RGB)
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_color_t;

// LED configuration
typedef struct {
    uint8_t gpio_pin;        // GPIO pin for LED data
    uint16_t num_leds;       // Number of LEDs in strip
    uint8_t brightness;      // Global brightness (0-255)
    led_effect_t effect;     // Current effect
    led_color_t color1;      // Primary color
    led_color_t color2;      // Secondary color
    uint8_t speed;           // Effect speed (0-255)
    uint8_t sensitivity;     // Audio sensitivity (0-255)
    uint8_t color_order;     // 0 = RGB, 1 = GRB
    uint8_t ignore_volume;   // 0 = follow playback volume, 1 = auto-normalize input
    uint8_t bass_focus;      // 0 = full-spectrum, 255 = strongly bass-focused
    uint8_t auto_rainbow;    // 0 = use static color1, 1 = cycle color1 through rainbow
    uint8_t rainbow_speed;   // 0-255: speed of auto-rainbow hue animation for color1
    uint8_t auto_rainbow2;   // 0 = use static color2, 1 = cycle color2 through rainbow
    uint8_t rainbow_speed2;  // 0-255: speed of auto-rainbow hue animation for color2
} led_config_t;

// Initialize LED controller
esp_err_t led_controller_init(led_config_t *config);

// Deinitialize LED controller
esp_err_t led_controller_deinit(void);

// Update configuration
esp_err_t led_controller_set_effect(led_effect_t effect);
esp_err_t led_controller_set_brightness(uint8_t brightness);
esp_err_t led_controller_set_color(led_color_t color1, led_color_t color2);
esp_err_t led_controller_set_speed(uint8_t speed);
esp_err_t led_controller_set_sensitivity(uint8_t sensitivity);
esp_err_t led_controller_set_bass_focus(uint8_t bass_focus);
esp_err_t led_controller_set_auto_rainbow(uint8_t enabled);
esp_err_t led_controller_set_rainbow_speed(uint8_t speed);
esp_err_t led_controller_set_auto_rainbow2(uint8_t enabled);
esp_err_t led_controller_set_rainbow_speed2(uint8_t speed);
esp_err_t led_controller_get_config(led_config_t *config);

// Feed audio data for reactive effects
// audio_data: stereo interleaved int16 samples
// num_samples: number of samples per channel
esp_err_t led_controller_feed_audio(const int16_t *audio_data, size_t num_samples);

// Lightweight path for sources that only have a volume level
// (e.g. Bluetooth A2DP absolute volume). level should be 0.0 - 1.0.
// This updates the internal energy and silence detection so effects
// react even when raw PCM samples are not available.
esp_err_t led_controller_feed_level(float level);

#ifdef __cplusplus
}
#endif

#endif // LED_CONTROLLER_H
