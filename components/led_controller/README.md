# Sound-Reactive LED Controller Component

## Component Overview

Minimal sound-reactive LED controller for ESP32-based Snapcast clients. Provides 10 built-in effects controlled via HTTP API with <100 KB flash footprint.

## Features
- Native ESP-IDF (no Arduino)
- Hardware RMT for WS2812B/SK6812
- 10 audio-reactive effects
- HTTP REST API
- Web UI integration
- Real-time audio analysis
- <100 KB flash, ~5 KB RAM

## Dependencies
- ESP-IDF v4.4+
- Components: driver, esp_timer, freertos
- Hardware: WS2812B or SK6812 LED strip

## Configuration
Available via `idf.py menuconfig` under "LED Controller Configuration":
- GPIO pin
- Number of LEDs  
- Default effect/brightness/sensitivity
- Default animation speed

## API
See led_controller.h for full API:
- `led_controller_init()` - Initialize with config
- `led_controller_set_effect()` - Change effect
- `led_controller_set_brightness()` - Adjust brightness
- `led_controller_feed_audio()` - Feed audio samples

## Integration
See LED_INTEGRATION.md and LED_QUICK_START.md for step-by-step instructions.

## License
Part of ESP32 Snapcast client project. Use freely.

## Author
Created for minimal overhead sound-reactive LEDs on ESP32.
