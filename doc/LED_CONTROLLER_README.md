# Sound-Reactive LED Controller for ESP32 Snapcast Client

A lightweight, native ESP-IDF LED controller that adds sound-reactive LED effects to your Snapcast client without the bloat of WLED.

## Features

✨ **10 Built-in Effects:**
- VU Meter (classic audio level bar)
- Spectrum Analyzer (bass/mid/treble bars)
- Pulse (breathing with audio)
- Wave (traveling wave effect)
- Energy Bar (peak-hold energy display)
- Rainbow Pulse (colorful audio reactive)
- Beat Flash (flash on beats)
- Bass Pulse (bass-reactive pulse)
- Stereo VU (dual VU meters from center)
- Solid Color (static color)

🚀 **Performance:**
- Native ESP-IDF (no Arduino bloat)
- Hardware-accelerated WS2812B/SK6812 via RMT
- <100 KB flash usage
- Real-time audio analysis
- ~30 FPS LED updates

🎛️ **HTTP Control:**
- Web-based control panel
- Real-time effect switching
- Brightness, speed, and sensitivity control
- Custom color selection
- JSON API for automation

## Hardware Requirements

- ESP32 (any variant with RMT support)
- WS2812B or SK6812 LED strip
- 5V power supply for LEDs (separate from ESP32 if >10 LEDs)
- 1x 470Ω resistor (data line protection, optional but recommended)

### Wiring

```
ESP32 GPIO Pin → [470Ω] → LED Strip DIN
LED Strip GND  → Common Ground
LED Strip 5V   → External 5V Power Supply
```

**Power Considerations:**
- Each LED draws ~60mA at full white brightness
- 30 LEDs × 60mA = 1.8A max
- Use external power supply for >10 LEDs
- Connect ESP32 GND to LED power supply GND

## Quick Start

### 1. Configure

Run menuconfig:
```bash
idf.py menuconfig
```

Navigate to `LED Controller Configuration` and set:
- Enable LED Controller: `[*]`
- LED Data GPIO Pin: `5` (or your preferred pin)
- Number of LEDs: `30` (or your strip length)
- Default Brightness: `128` (0-255)
- Default Effect: `1` (VU Meter)
- Default Sensitivity: `150` (0-255)

### 2. Integrate into main.c

Add to your `main/main.c`:

```c
// Add include at top
#ifdef CONFIG_ENABLE_LED_CONTROLLER
#include "led_controller.h"
#endif

// Add global variable
#ifdef CONFIG_ENABLE_LED_CONTROLLER
static bool led_initialized = false;
#endif

// Add initialization function
#ifdef CONFIG_ENABLE_LED_CONTROLLER
static void init_led_controller(void)
{
    led_config_t led_config = {
        .gpio_pin = CONFIG_LED_GPIO_PIN,
        .num_leds = CONFIG_LED_NUM_LEDS,
        .brightness = CONFIG_LED_DEFAULT_BRIGHTNESS,
        .effect = CONFIG_LED_DEFAULT_EFFECT,
        .color1 = {.r = 255, .g = 0, .b = 0},
        .color2 = {.r = 0, .g = 0, .b = 255},
        .speed = CONFIG_LED_DEFAULT_SPEED,
        .sensitivity = CONFIG_LED_DEFAULT_SENSITIVITY,
    };
    
    if (led_controller_init(&led_config) == ESP_OK) {
        ESP_LOGI("LED", "LED controller initialized");
        led_initialized = true;
    }
}
#endif

// Call in app_main() after network init
#ifdef CONFIG_ENABLE_LED_CONTROLLER
    init_led_controller();
#endif

// Feed audio data (add after dsp_processor_worker calls)
#ifdef CONFIG_ENABLE_LED_CONTROLLER
if (led_initialized && pcmData && pcmData->fragment->payload) {
    led_controller_feed_audio((const int16_t*)pcmData->fragment->payload,
                             pcmData->fragment->size / 4);
}
#endif
```

See [LED_INTEGRATION.md](LED_INTEGRATION.md) for detailed integration instructions.

### 3. Build and Flash

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

### 4. Access Web UI

1. Connect to your ESP32's IP address in a browser
2. Replace `html/index.html` with `html/index_with_leds.html` (or merge the LED controls)
3. The LED controller section will appear automatically if enabled
4. Adjust settings in real-time!

## HTTP API

### Get LED Status
```bash
GET /led
```

Response:
```json
{
  "effect": 1,
  "brightness": 128,
  "speed": 128,
  "sensitivity": 150,
  "color": {"r": 255, "g": 0, "b": 0},
  "num_leds": 30
}
```

### Update LED Settings
```bash
POST /led
Content-Type: application/x-www-form-urlencoded

effect=1&brightness=200&speed=150&sensitivity=180&color=0,255,0
```

### Control via curl

```bash
# Set to VU meter effect
curl -X POST http://192.168.1.100/led -d "effect=1"

# Adjust brightness
curl -X POST http://192.168.1.100/led -d "brightness=200"

# Change to rainbow pulse with high sensitivity
curl -X POST http://192.168.1.100/led -d "effect=6&sensitivity=200"

# Set custom color for pulse effect
curl -X POST http://192.168.1.100/led -d "effect=3&color=255,0,255"
```

## Effect Guide

| Effect | Description | Best For |
|--------|-------------|----------|
| **VU Meter** | Classic audio level bar | General music, clear visualization |
| **Spectrum Analyzer** | 3-band frequency display | Electronic, bass-heavy music |
| **Pulse** | Breathing effect synced to audio | Ambient, chill music |
| **Wave** | Traveling wave along strip | Dynamic, rhythmic tracks |
| **Energy Bar** | Peak-hold energy display | High-energy music |
| **Rainbow Pulse** | Color-cycling with audio | Parties, colorful displays |
| **Beat Flash** | Flashes on beat detection | EDM, dance music |
| **Bass Pulse** | Bass-focused pulse | Hip-hop, dubstep |
| **Stereo VU** | Dual VU from center | Stereo music, symmetry |
| **Solid Color** | Static color (no reaction) | Ambient lighting |

## Tuning Tips

### Sensitivity
- **Low (50-100):** Subtle reactions, good for background music
- **Medium (100-180):** Balanced response for most music
- **High (180-255):** Aggressive response for loud/energetic music

### Speed
- **Low (20-80):** Slow, smooth animations
- **Medium (80-180):** Moderate animation speed
- **High (180-255):** Fast, energetic animations

### Brightness
- **Low (30-80):** Dimmed, ambient lighting
- **Medium (80-180):** Comfortable viewing
- **High (180-255):** Maximum visibility (high power consumption)

## Troubleshooting

### LEDs don't light up
- Check wiring (data pin, ground, 5V)
- Verify GPIO pin in menuconfig matches your wiring
- Ensure external 5V power for >10 LEDs
- Check LED strip type (WS2812B vs SK6812)

### LEDs flicker or show wrong colors
- Add 470Ω resistor on data line
- Keep data wire short (<1m) or use shielded cable
- Add 1000µF capacitor across LED power supply
- Reduce brightness if power supply is insufficient

### No audio reaction
- Verify LED controller is receiving audio (check logs)
- Adjust sensitivity higher
- Ensure Snapcast is playing audio
- Check that `led_controller_feed_audio()` is being called

### Effects are too fast/slow
- Adjust speed parameter via web UI
- Some effects ignore speed (VU meter, spectrum)

### High CPU usage
- Reduce number of LEDs
- Lower update rate in led_controller.c (increase frame_delay)

## Memory Usage

Approximate flash/RAM usage:
- **Flash:** ~60-80 KB (component code)
- **RAM:** ~5 KB + (num_leds × 3 bytes) for LED buffer
  - 30 LEDs = ~5 KB + 90 bytes
  - 60 LEDs = ~5 KB + 180 bytes
  - 100 LEDs = ~5 KB + 300 bytes

## Performance

- Update rate: ~30 FPS (33ms frame time)
- Audio latency: <50ms
- CPU usage: 2-5% on ESP32 @ 240MHz
- Works alongside Snapcast + Bluetooth without issues

## Advanced Customization

### Add Custom Effects

Edit `components/led_controller/led_controller.c`:

1. Add effect enum to `led_effect_t` in header
2. Create effect function (see existing examples)
3. Add case to switch statement in `led_task()`
4. Update HTML UI effect dropdown

Example:
```c
static void effect_strobe(void)
{
    static uint8_t state = 0;
    float energy = g_energy_avg * (g_config.sensitivity / 255.0f);
    
    if (energy > 0.5f) {
        state = !state;
    }
    
    uint8_t val = state ? 255 : 0;
    for (int i = 0; i < g_config.num_leds; i++) {
        set_led(i, val, val, val);
    }
}
```

### Adjust Audio Analysis

Modify `led_controller_feed_audio()` to:
- Add FFT for true spectrum analysis
- Implement beat detection algorithms
- Add frequency band filters

## License

This component is provided as-is for use with the ESP32 Snapcast client project.

## Credits

- WS2812B RMT driver based on ESP-IDF examples
- Audio analysis inspired by various music visualizer projects
- Built for minimal overhead on resource-constrained ESP32

---

**Enjoy your sound-reactive LEDs!** 🎵💡
