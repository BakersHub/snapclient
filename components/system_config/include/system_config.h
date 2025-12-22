#ifndef SYSTEM_CONFIG_H
#define SYSTEM_CONFIG_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYSTEM_CONFIG_MAX_NAME_LEN 32
#define SYSTEM_CONFIG_MAX_HOST_LEN 64
#define SYSTEM_CONFIG_MAX_SSID_LEN 32
#define SYSTEM_CONFIG_MAX_PASS_LEN 64

typedef struct {
    bool  volume_buttons_enabled;
    int   volume_up_pin;
    int   volume_down_pin;
    bool  effect_button_enabled;
    int   effect_button_pin;
    char  snapclient_name[SYSTEM_CONFIG_MAX_NAME_LEN];
    float snapcast_gain_boost;
    char  wifi_ssid[SYSTEM_CONFIG_MAX_SSID_LEN];
    char  wifi_password[SYSTEM_CONFIG_MAX_PASS_LEN];
    char  snapserver_host[SYSTEM_CONFIG_MAX_HOST_LEN];
    int   snapserver_port;
    bool  sh1106_enabled;
    int   sh1106_sda_gpio;
    int   sh1106_scl_gpio;
    int   sh1106_i2c_freq_hz;
    int   sh1106_column_offset;   // 0 for 0.96" panels, 2 for 1.28" SH1106
    // Runtime-overridable audio pins (override Kconfig defaults when set)
    int   i2s_mclk_pin;
    int   i2s_bck_pin;
    int   i2s_lrck_pin;
    int   i2s_dataout_pin;
    int   pcm5102a_mute_pin;
} system_config_t;

void system_config_set_defaults(system_config_t *config);
esp_err_t system_config_load_from_nvs(system_config_t *config);
esp_err_t system_config_save_to_nvs(const system_config_t *config);

#ifdef __cplusplus
}
#endif

#endif // SYSTEM_CONFIG_H
