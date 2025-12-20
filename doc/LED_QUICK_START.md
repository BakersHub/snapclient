# LED Controller Quick Start Guide

## 5-Minute Setup

### 1. Enable in menuconfig (1 min)
```bash
idf.py menuconfig
```
Navigate to: **Component config → LED Controller Configuration**

Set:
- [x] Enable LED Controller
- LED Data GPIO Pin: **5** (or your choice)
- Number of LEDs: **30** (your strip length)
- Default Brightness: **128**
- Default Effect: **1** (VU Meter)
- Default Sensitivity: **150**

Save and exit (S, then Enter, then Q)

### 2. Add to main.c (2 min)

**Add at top of main.c (after other includes):**
```c
#ifdef CONFIG_ENABLE_LED_CONTROLLER
#include "led_controller.h"
static bool led_initialized = false;

static void init_led_controller(void)
{
    led_config_t cfg = {
        .gpio_pin = CONFIG_LED_GPIO_PIN,
        .num_leds = CONFIG_LED_NUM_LEDS,
        .brightness = CONFIG_LED_DEFAULT_BRIGHTNESS,
        .effect = CONFIG_LED_DEFAULT_EFFECT,
        .color1 = {255, 0, 0},
        .color2 = {0, 0, 255},
        .speed = CONFIG_LED_DEFAULT_SPEED,
        .sensitivity = CONFIG_LED_DEFAULT_SENSITIVITY,
    };
    led_initialized = (led_controller_init(&cfg) == ESP_OK);
}
#endif
```

**In app_main(), after network initialization:**
```c
#ifdef CONFIG_ENABLE_LED_CONTROLLER
    init_led_controller();
#endif
```

**After EACH dsp_processor_worker() call (find 3 locations):**
```c
#ifdef CONFIG_ENABLE_LED_CONTROLLER
if (led_initialized && pcmData && pcmData->fragment->payload) {
    led_controller_feed_audio((const int16_t*)pcmData->fragment->payload,
                             pcmData->fragment->size / 4);
}
#endif
```

### 3. Build & Flash (2 min)
```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### 4. Wire LEDs
```
ESP32 GPIO 5  →  [470Ω resistor]  →  LED Strip DIN
ESP32 GND     →  LED Strip GND
Power Supply 5V → LED Strip 5V
Power Supply GND → LED Strip GND (and ESP32 GND)
```

### 5. Test!
1. Play music through Snapcast
2. LEDs should react to audio
3. Visit ESP32 IP in browser
4. LED controls appear in web UI
5. Try different effects!

---

## Web UI Alternative

Instead of modifying the existing HTML, you can:

```bash
# Backup original
mv html/index.html html/index_backup.html

# Use new version with LED controls
cp html/index_with_leds.html html/index.html

# Rebuild to update SPIFFS
idf.py build flash
```

---

## HTTP Control Examples

```bash
# Get status
curl http://192.168.1.100/led

# VU meter, max brightness
curl -X POST http://192.168.1.100/led -d "effect=1&brightness=255"

# Rainbow pulse, high sensitivity
curl -X POST http://192.168.1.100/led -d "effect=6&sensitivity=220"

# Purple pulse effect
curl -X POST http://192.168.1.100/led -d "effect=3&color=255,0,255"

# Turn off
curl -X POST http://192.168.1.100/led -d "effect=0"
```

---

## Common Issues

**LEDs don't work:**
- Check GPIO pin number matches your wiring
- Ensure 5V external power for >10 LEDs
- Try GPIO 18, 19, or 23 if GPIO 5 doesn't work

**No audio reaction:**
- Increase sensitivity to 200-250
- Play louder music
- Check that Snapcast is actually playing

**Build errors:**
- Run `idf.py fullclean` then `idf.py build`
- Verify all new files are in correct directories

---

## Effect Cheat Sheet

| Effect # | Name | Best For |
|----------|------|----------|
| 0 | Off | Turn off LEDs |
| 1 | VU Meter | General music |
| 2 | Spectrum | Bass-heavy tracks |
| 3 | Pulse | Ambient/chill |
| 4 | Wave | Rhythmic music |
| 5 | Energy Bar | High-energy |
| 6 | Rainbow Pulse | Colorful party |
| 7 | Beat Flash | EDM/dance |
| 8 | Bass Pulse | Hip-hop/dubstep |
| 9 | Stereo VU | Stereo effects |
| 10 | Solid Color | Static ambient |

---

That's it! You now have sound-reactive LEDs running on your Snapcast client! 🎵💡
