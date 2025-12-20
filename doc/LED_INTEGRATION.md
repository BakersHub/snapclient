/* LED Controller Integration Example for main.c
 * 
 * This file shows how to integrate the LED controller into your Snapcast client.
 * Add these changes to your main/main.c file.
 */

// 1. Add at the top with other includes (around line 60-75):
#ifdef CONFIG_ENABLE_LED_CONTROLLER
#include "led_controller.h"
#endif

// 2. Add global variable after other globals (around line 150-170):
#ifdef CONFIG_ENABLE_LED_CONTROLLER
static bool led_initialized = false;
#endif

// 3. Add LED initialization function (around line 200-300):
#ifdef CONFIG_ENABLE_LED_CONTROLLER
static void init_led_controller(void)
{
    led_config_t led_config = {
        .gpio_pin = CONFIG_LED_GPIO_PIN,
        .num_leds = CONFIG_LED_NUM_LEDS,
        .brightness = CONFIG_LED_DEFAULT_BRIGHTNESS,
        .effect = CONFIG_LED_DEFAULT_EFFECT,
        .color1 = {.r = 255, .g = 0, .b = 0},  // Red default
        .color2 = {.r = 0, .g = 0, .b = 255},  // Blue secondary
        .speed = CONFIG_LED_DEFAULT_SPEED,
        .sensitivity = CONFIG_LED_DEFAULT_SENSITIVITY,
    };
    
    esp_err_t ret = led_controller_init(&led_config);
    if (ret == ESP_OK) {
        ESP_LOGI("LED", "LED controller initialized: %d LEDs on GPIO %d", 
                 led_config.num_leds, led_config.gpio_pin);
        led_initialized = true;
    } else {
        ESP_LOGE("LED", "Failed to initialize LED controller: %s", 
                 esp_err_to_name(ret));
    }
}
#endif

// 4. Call init_led_controller() in app_main() after network is initialized
//    (Look for where init_http_server_task() is called and add after it):
#ifdef CONFIG_ENABLE_LED_CONTROLLER
    init_led_controller();
#endif

// 5. Feed audio to LEDs - Add after dsp_processor_worker() calls
//    (There are multiple locations around lines 1448, 1559, 1621)
//    
//    Find blocks like this:
//    
//    #if CONFIG_USE_DSP_PROCESSOR
//    if ((pcmData) && (pcmData->fragment->payload)) {
//        dsp_processor_worker(pcmData->fragment->payload,
//                            pcmData->fragment->size,
//                            scSet.sr);
//    }
//    #endif
//
//    And add after them:
//
//    #ifdef CONFIG_ENABLE_LED_CONTROLLER
//    if (led_initialized && pcmData && pcmData->fragment->payload) {
//        // Feed audio to LED controller
//        // pcmData->fragment->payload is char*, need to cast to int16_t*
//        led_controller_feed_audio((const int16_t*)pcmData->fragment->payload,
//                                 pcmData->fragment->size / 4); // size/4 = samples per channel (stereo)
//    }
//    #endif

// Example of complete block:
/*
#if CONFIG_USE_DSP_PROCESSOR
if ((pcmData) && (pcmData->fragment->payload)) {
    dsp_processor_worker(pcmData->fragment->payload,
                        pcmData->fragment->size,
                        scSet.sr);
}
#endif

#ifdef CONFIG_ENABLE_LED_CONTROLLER
if (led_initialized && pcmData && pcmData->fragment->payload) {
    led_controller_feed_audio((const int16_t*)pcmData->fragment->payload,
                             pcmData->fragment->size / 4);
}
#endif
*/

// 6. Update ui_http_server CMakeLists.txt to include led_controller dependency:
//    Edit: components/ui_http_server/CMakeLists.txt
//    Change the REQUIRES line to:
//    REQUIRES driver esp_timer freertos led_controller

/* That's it! The LED controller will now:
 * - Initialize on startup with Kconfig settings
 * - Receive audio data and create reactive effects
 * - Be controllable via HTTP API at /led
 * - Show controls in the web UI (use index_with_leds.html)
 */
