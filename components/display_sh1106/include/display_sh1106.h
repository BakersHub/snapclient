#ifndef DISPLAY_SH1106_H
#define DISPLAY_SH1106_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Display dimensions
#define SH1106_WIDTH 128
#define SH1106_HEIGHT 64

// I2C configuration
#define SH1106_I2C_PORT I2C_NUM_0

#ifdef CONFIG_SH1106_I2C_SDA_GPIO
#define SH1106_I2C_SDA_GPIO CONFIG_SH1106_I2C_SDA_GPIO
#else
#define SH1106_I2C_SDA_GPIO 21
#endif

#ifdef CONFIG_SH1106_I2C_SCL_GPIO
#define SH1106_I2C_SCL_GPIO CONFIG_SH1106_I2C_SCL_GPIO
#else
#define SH1106_I2C_SCL_GPIO 22
#endif

#ifdef CONFIG_SH1106_I2C_FREQ_HZ
#define SH1106_I2C_FREQ_HZ CONFIG_SH1106_I2C_FREQ_HZ
#else
#define SH1106_I2C_FREQ_HZ 400000
#endif

#ifdef CONFIG_SH1106_I2C_ADDRESS
#define SH1106_I2C_ADDRESS CONFIG_SH1106_I2C_ADDRESS
#else
#define SH1106_I2C_ADDRESS 0x3C
#endif

// Panel-dependent offsets
#ifdef CONFIG_SH1106_DISPLAY_OFFSET
#define SH1106_DISPLAY_OFFSET CONFIG_SH1106_DISPLAY_OFFSET
#else
#define SH1106_DISPLAY_OFFSET 0
#endif

#ifdef CONFIG_SH1106_COLUMN_OFFSET
#define SH1106_COLUMN_OFFSET CONFIG_SH1106_COLUMN_OFFSET
#else
#define SH1106_COLUMN_OFFSET 2
#endif

// Display layout zones
#define METADATA_ZONE_HEIGHT 32
#define EQ_ZONE_HEIGHT 32
// Number of EQ bars drawn on the SH1106 display
#define EQ_BARS 8
// Widen bars and spacing so 8 bars span roughly the
// same width the original 16 bars did
#define EQ_BAR_WIDTH 12
#define EQ_BAR_SPACING 3

// Song metadata structure
typedef struct {
    char title[64];
    char artist[64];
    char album[32];
    char genre[32];
    bool title_updated;
    bool artist_updated;
    bool album_updated;
    bool genre_updated;
    uint32_t scroll_position;
    uint32_t last_scroll_time;
} song_metadata_t;

// Audio visualization data
typedef struct {
    uint8_t eq_levels[EQ_BARS];
    uint8_t volume_level;      // 0-100
    uint8_t peak_level;        // 0-100
    bool is_playing;
    bool is_connected;
    bool is_resyncing;         // True when in RESYNCING HARD state
    uint32_t last_update_time;
} audio_viz_t;

// WiFi signal strength data
typedef struct {
    int32_t rssi;              // Signal strength in dBm
    uint8_t signal_bars;       // 0-4 bars
    bool wifi_connected;       // WiFi connection status
    uint32_t last_update_time;
} wifi_signal_t;

// Bluetooth signal strength data
typedef struct {
    int32_t rssi;              // Signal strength in dBm
    uint8_t signal_bars;       // 0-4 bars
    bool bt_connected;         // Bluetooth connection status
    uint32_t last_update_time;
} bt_signal_t;

// Display functions
// Default init using Kconfig / compiled-in I2C settings
esp_err_t display_init(void);
// Init with explicit I2C configuration (used by runtime system config)
esp_err_t display_init_with_i2c(int sda_gpio, int scl_gpio, int freq_hz);
void display_set_column_offset(int offset);
esp_err_t display_clear(void);
esp_err_t display_update(void);

// Content functions
void display_set_song_metadata(const char* title, const char* artist, const char* album, const char* genre);
void display_set_audio_levels(const uint8_t* eq_levels, uint8_t volume, bool is_playing);
void display_set_connection_status(bool connected);
void display_set_resyncing(bool resyncing);
void display_set_device_name(const char* device_name);
void display_set_wifi_signal(int32_t rssi, bool wifi_connected);
void display_set_bt_signal(bool connected, int32_t rssi);
void display_set_bt_device_name(const char* device_name);
void display_set_bt_audio_playing(bool playing);
void display_set_snapcast_volume(uint8_t volume);
void display_set_snapcast_mute(bool muted);

// Task management
void display_task_start(void);
void display_task_stop(void);

#endif // DISPLAY_SH1106_H