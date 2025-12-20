# SH1106 Display "NO WIFI" Fix Instructions

## Problem
The display is stuck showing "NO WIFI" because the initial display setup is incomplete. The WiFi signal monitor task exists and runs, but the display needs proper initialization.

## Root Cause
There are two issues:

1. **Incomplete display initialization** around line 2571 in main.c
2. **WiFi signal monitor starts before WiFi is connected**

## Solution

### Step 1: Fix the display initialization (around line 2571)

Find this code in `main/main.c` (around line 2571):

```c
#if CONFIG_ENABLE_SH1106_DISPLAY
  ESP_LOGI(TAG, "Initializing SH1106 display...");
  esp_err_t display_ret = display_init();
  if (display_ret == ESP_OK) {
    ESP_LOGI(TAG, "SH1106 display initialized successfully");
    display_set_device_name(SNAPCAST_CLIENT_NAME);
    display_task_start();
  } else {
    ESP_LOGE(TAG, "Failed to initialize SH1106 display: %s", esp_err_to_name(display_ret));        
  }
#endif
```

Replace it with:

```c
#if CONFIG_ENABLE_SH1106_DISPLAY
  ESP_LOGI("DUAL_OTA", "Initializing SH1106 OLED display");
  esp_err_t display_ret = display_init();
  if (display_ret == ESP_OK) {
    ESP_LOGI("DUAL_OTA", "SH1106 display initialized successfully");
    
    // Clear display to remove any previous state
    ESP_LOGI("DUAL_OTA", "Clearing display and starting display task");
    display_clear();
    
    // Start the display task (crucial for screen updates!)
    display_task_start();
    
    // Set the local device name on the display
    display_set_device_name(LOCAL_DEVICE_NAME);
    
    // Small delay to let display task initialize
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Set initial metadata for snapclient mode
    display_set_song_metadata("Snapclient", "Starting up...", "", "");
    
    // Set initial audio status with all bars at baseline level
    uint8_t initial_eq_levels[16] = {20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20};
    display_set_audio_levels(initial_eq_levels, 50, false);
    display_set_connection_status(false);
    
    // Force an immediate display update
    display_update();
    ESP_LOGI("DUAL_OTA", "Snapclient display content set and updated");
  } else {
    ESP_LOGW("DUAL_OTA", "Failed to initialize SH1106 display: %s", esp_err_to_name(display_ret));
  }
#endif
```

### Step 2: Verify WiFi signal monitoring task is started

The WiFi signal monitoring task (`wifi_signal_monitor_task`) should be created AFTER WiFi is initialized. Look for this code around line 3476 in main.c:

```c
  // Start WiFi signal monitoring task
  ESP_LOGI("DUAL_OTA", "Starting WiFi signal monitoring task");
  xTaskCreate(wifi_signal_monitor_task, "wifi_signal", 3072, NULL, 2, &wifi_monitor_task_handle);
  ESP_LOGI("DUAL_OTA", "WiFi signal monitoring started");
```

Make sure this comes AFTER the `wifi_init()` call in app_main().

### Step 3: Build and test

```bash
idf.py build
idf.py flash
idf.py monitor
```

## Expected Behavior After Fix

1. **On startup**: Display shows "Snapclient - Starting up..." with signal bars at 0
2. **After WiFi connects**: Display shows WiFi signal strength bars
3. **When audio plays**: Display shows song metadata and EQ visualization
4. **If WiFi disconnects**: Display shows "NO WIFI"

## Key Functions That Update the Display

From the working firmware, these functions update the display:

- `display_set_wifi_signal(rssi, connected)` - Updates WiFi signal bars
- `display_set_song_metadata(title, artist, album, genre)` - Updates song info  
- `display_set_audio_levels(eq_levels, volume, is_playing)` - Updates EQ bars
- `display_set_connection_status(connected)` - Updates connection status
- `display_update()` - Forces immediate screen refresh

The WiFi signal monitor task automatically calls `display_set_wifi_signal()` every 3 seconds once WiFi is connected.

## Debug Tips

Watch for these log messages:

```
DUAL_OTA: SH1106 display initialized successfully
DUAL_OTA: Snapclient display content set and updated
SC: WiFi signal monitor task started
SC: WiFi connected, starting signal monitoring
SC: WiFi RSSI: -XX dBm (Good/Fair/Poor)
```

If you don't see "WiFi connected, starting signal monitoring", the WiFi signal monitor is waiting for WiFi to connect first (this is correct behavior).
