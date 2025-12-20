# LED Configuration System - NVS Runtime Configuration

## Overview
The LED controller now supports runtime configuration changes via the web interface. Settings are saved to NVS (Non-Volatile Storage) and persist across reboots. The ESP32 restarts automatically after saving configuration, applying the new settings without requiring reflashing.

## What Changed

### 1. New NVS Storage Layer
Created `components/led_controller/led_config_nvs.c` and `led_config_nvs.h`:
- `led_config_save_to_nvs()` - Saves configuration to NVS
- `led_config_load_from_nvs()` - Loads configuration from NVS
- `led_config_get_defaults()` - Returns Kconfig defaults

### 2. Updated main.c Initialization
Modified `init_led_controller()` to:
1. Try loading configuration from NVS first
2. Fall back to Kconfig defaults if no NVS config exists
3. Log which source was used (NVS or Kconfig)

### 3. Updated HTTP Handler
Modified `/led/config` endpoint in `ui_http_server.c` to:
- Parse all LED configuration parameters (num_leds, gpio_pin, brightness, effect, speed, sensitivity)
- Save to NVS
- Restart the ESP32 via `esp_restart()`

### 4. Enhanced HTML Interface
Rebuilt `components/ui_http_server/html/index.html` with two sections:

#### Persistent Configuration Section
- Number of LEDs (1-1000)
- GPIO Pin (0-39)
- Default Brightness (0-255)
- Default Effect (0-10)
- Default Speed (0-255)
- Default Sensitivity (0-255)
- "Save Configuration & Restart" button

#### Live Controls Section (runtime, not saved)
- Effect selector
- Brightness slider
- Speed slider
- Sensitivity slider
- Color picker

## Configuration Structure
```c
typedef struct {
    uint16_t num_leds;
    uint8_t gpio_pin;
    uint8_t brightness;
    uint8_t effect;
    uint8_t speed;
    uint8_t sensitivity;
    uint8_t color_r, color_g, color_b;
} led_config_nvs_t;
```

## How It Works

### First Boot (No NVS Config)
1. ESP32 starts
2. `init_led_controller()` calls `led_config_load_from_nvs()`
3. Returns `ESP_ERR_NOT_FOUND`
4. Falls back to Kconfig defaults
5. Initializes LED controller with Kconfig values
6. Logs: "No NVS config found, using Kconfig defaults"

### Changing Configuration
1. User opens web interface at `http://<esp-ip>/`
2. Modifies settings in "Persistent Configuration" section
3. Clicks "Save Configuration & Restart"
4. JavaScript sends POST to `/led/config` with all parameters
5. ESP32 saves to NVS and calls `esp_restart()`
6. ESP32 reboots and loads new config from NVS
7. LEDs initialize with new settings

### Subsequent Boots
1. ESP32 starts
2. `init_led_controller()` calls `led_config_load_from_nvs()`
3. Returns `ESP_OK` with saved configuration
4. Initializes LED controller with NVS values
5. Logs: "Loading LED config from NVS: X LEDs on GPIO Y"

## Usage

### Web Interface
1. Navigate to `http://<esp-ip>/`
2. Scroll to "Sound-Reactive LEDs" section
3. Adjust "Persistent Configuration" settings
4. Click "Save Configuration & Restart"
5. Wait ~3-5 seconds for ESP32 to reboot
6. New settings will be active

### Testing
```bash
# Flash the firmware
idf.py flash monitor

# Open browser and test:
# 1. Change LED count from 100 to 50
# 2. Click "Save Configuration & Restart"
# 3. Monitor shows: "Loading LED config from NVS: 50 LEDs on GPIO 23"
# 4. Settings persist after power cycle
```

## API Endpoints

### GET /led
Returns current LED status (runtime state)
```json
{
  "effect": 8,
  "brightness": 128,
  "speed": 128,
  "sensitivity": 150,
  "num_leds": 100,
  "color": {"r": 255, "g": 0, "b": 0}
}
```

### POST /led
Updates live LED settings (not saved to NVS)
```
effect=8&brightness=200&speed=150&sensitivity=180&color=255,0,0
```

### POST /led/config
Saves persistent configuration and restarts ESP32
```
num_leds=50&gpio_pin=23&brightness=128&effect=1&speed=128&sensitivity=150
```

## NVS Namespace
- Namespace: `led_config`
- Keys: `num_leds`, `gpio_pin`, `brightness`, `effect`, `speed`, `sensitivity`, `color_r`, `color_g`, `color_b`

## Next Steps - Expanding to Other Settings

This LED configuration system serves as a template for adding runtime configuration to:

### 1. WiFi Settings
- SSID
- Password
- Static IP configuration

### 2. DAC Configuration
- GPIO pin assignments
- DAC model selection
- I2S configuration

### 3. Volume Controls
- Volume gain
- GPIO button assignments
- Volume step size

### 4. System Settings
- Device name
- Snapcast server address
- Audio buffer size

Each will follow the same pattern:
1. Create `xxx_config_nvs.c/h` with save/load functions
2. Update initialization code to load from NVS
3. Add HTTP handler for POST `/xxx/config`
4. Add HTML form section
5. Call `esp_restart()` after saving

## Build Instructions
```bash
# Clean build recommended
idf.py fullclean
idf.py build
idf.py flash monitor
```

## Reverting to Kconfig Defaults
To clear NVS and revert to Kconfig defaults:
```bash
idf.py erase-flash
idf.py flash monitor
```

Or via code, add a reset endpoint:
```c
nvs_handle_t handle;
nvs_open("led_config", NVS_READWRITE, &handle);
nvs_erase_all(handle);
nvs_commit(handle);
nvs_close(handle);
esp_restart();
```

## Files Modified

### Created
- `components/led_controller/led_config_nvs.c` (73 lines)
- `components/led_controller/include/led_config_nvs.h` (29 lines)
- `components/ui_http_server/html/index.html` (fresh rebuild)

### Modified
- `components/led_controller/CMakeLists.txt` (added nvs_flash dependency)
- `components/ui_http_server/ui_http_server.c` (updated `/led/config` handler)
- `main/main.c` (updated `init_led_controller()` to load from NVS)

## Configuration Priority
1. **NVS** (if exists) - Highest priority
2. **Kconfig** (if no NVS) - Fallback default

This allows:
- Fresh installations use Kconfig defaults
- User changes via web interface override Kconfig
- Kconfig still useful for factory defaults/mass deployment
