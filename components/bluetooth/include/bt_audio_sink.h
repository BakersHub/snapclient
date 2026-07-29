#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void bt_audio_sink_init();
bool bt_audio_sink_is_connected();

// Set Bluetooth absolute volume using AVRCP (percent 0-100)
void bt_audio_set_absolute_volume_percent(int percent);

// Send AVRC passthrough volume commands (alternative method)
void bt_audio_volume_up();
void bt_audio_volume_down();

// Get current Bluetooth volume (0-100%)
int bt_audio_get_volume_percent();

// Check if WiFi is intentionally stopped for Bluetooth-only mode
bool bt_audio_is_wifi_intentionally_stopped();

#ifdef __cplusplus
}
#endif
