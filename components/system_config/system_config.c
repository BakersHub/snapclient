#include "system_config.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "SYS_CONFIG";
static const char *NVS_NAMESPACE_SYS = "syscfg";

void system_config_set_defaults(system_config_t *config)
{
    if (!config) return;

    memset(config, 0, sizeof(*config));

#ifdef CONFIG_ENABLE_VOLUME_BUTTONS
    config->volume_buttons_enabled = true;
#else
    config->volume_buttons_enabled = false;
#endif

#ifdef CONFIG_VOLUME_UP_PIN
    config->volume_up_pin = CONFIG_VOLUME_UP_PIN;
#else
    config->volume_up_pin = 27;
#endif

#ifdef CONFIG_VOLUME_DOWN_PIN
    config->volume_down_pin = CONFIG_VOLUME_DOWN_PIN;
#else
    config->volume_down_pin = 14;
#endif

    // LED effect toggle button disabled by default, default GPIO 18
    config->effect_button_enabled = false;
    config->effect_button_pin = 18;

#ifdef CONFIG_SNAPCLIENT_NAME
    strncpy(config->snapclient_name, CONFIG_SNAPCLIENT_NAME, SYSTEM_CONFIG_MAX_NAME_LEN - 1);
    config->snapclient_name[SYSTEM_CONFIG_MAX_NAME_LEN - 1] = '\0';
#else
    strncpy(config->snapclient_name, "Snapclient", SYSTEM_CONFIG_MAX_NAME_LEN - 1);
    config->snapclient_name[SYSTEM_CONFIG_MAX_NAME_LEN - 1] = '\0';
#endif

#ifdef CONFIG_SNAPCAST_GAIN_BOOST
    config->snapcast_gain_boost = (float)atof(CONFIG_SNAPCAST_GAIN_BOOST);
    if (config->snapcast_gain_boost <= 0.0f) {
        config->snapcast_gain_boost = 1.0f;
    }
#else
    config->snapcast_gain_boost = 1.0f;
#endif

#ifdef CONFIG_WIFI_SSID
    strncpy(config->wifi_ssid, CONFIG_WIFI_SSID, SYSTEM_CONFIG_MAX_SSID_LEN - 1);
    config->wifi_ssid[SYSTEM_CONFIG_MAX_SSID_LEN - 1] = '\0';
#else
    config->wifi_ssid[0] = '\0';
#endif

#ifdef CONFIG_WIFI_PASSWORD
    strncpy(config->wifi_password, CONFIG_WIFI_PASSWORD, SYSTEM_CONFIG_MAX_PASS_LEN - 1);
    config->wifi_password[SYSTEM_CONFIG_MAX_PASS_LEN - 1] = '\0';
#else
    config->wifi_password[0] = '\0';
#endif

#ifdef CONFIG_SNAPSERVER_HOST
    strncpy(config->snapserver_host, CONFIG_SNAPSERVER_HOST, SYSTEM_CONFIG_MAX_HOST_LEN - 1);
    config->snapserver_host[SYSTEM_CONFIG_MAX_HOST_LEN - 1] = '\0';
#else
    strncpy(config->snapserver_host, "192.168.1.100", SYSTEM_CONFIG_MAX_HOST_LEN - 1);
    config->snapserver_host[SYSTEM_CONFIG_MAX_HOST_LEN - 1] = '\0';
#endif

#ifdef CONFIG_SNAPSERVER_PORT
    config->snapserver_port = CONFIG_SNAPSERVER_PORT;
#else
    config->snapserver_port = 1704;
#endif

#if CONFIG_ENABLE_SH1106_DISPLAY
    config->sh1106_enabled = true;
#else
    config->sh1106_enabled = false;
#endif

#ifdef CONFIG_SH1106_I2C_SDA_GPIO
    config->sh1106_sda_gpio = CONFIG_SH1106_I2C_SDA_GPIO;
#else
    config->sh1106_sda_gpio = 21;
#endif

#ifdef CONFIG_SH1106_I2C_SCL_GPIO
    config->sh1106_scl_gpio = CONFIG_SH1106_I2C_SCL_GPIO;
#else
    config->sh1106_scl_gpio = 22;
#endif

#ifdef CONFIG_SH1106_I2C_FREQ_HZ
    config->sh1106_i2c_freq_hz = CONFIG_SH1106_I2C_FREQ_HZ;
#else
    config->sh1106_i2c_freq_hz = 400000;
#endif

    // Default column offset depends on panel type; 2 for classic SH1106, 0 for SSD1306-style
#ifdef CONFIG_SH1106_COLUMN_OFFSET
    config->sh1106_column_offset = CONFIG_SH1106_COLUMN_OFFSET;
#else
    config->sh1106_column_offset = 2;
#endif

    // I2S master interface pins (override-able via web UI + NVS)
#ifdef CONFIG_MASTER_I2S_MCLK_PIN
    config->i2s_mclk_pin = CONFIG_MASTER_I2S_MCLK_PIN;
#else
    config->i2s_mclk_pin = 0;
#endif

#ifdef CONFIG_MASTER_I2S_BCK_PIN
    config->i2s_bck_pin = CONFIG_MASTER_I2S_BCK_PIN;
#else
    config->i2s_bck_pin = 33;
#endif

#ifdef CONFIG_MASTER_I2S_LRCK_PIN
    config->i2s_lrck_pin = CONFIG_MASTER_I2S_LRCK_PIN;
#else
    config->i2s_lrck_pin = 32;
#endif

#ifdef CONFIG_MASTER_I2S_DATAOUT_PIN
    config->i2s_dataout_pin = CONFIG_MASTER_I2S_DATAOUT_PIN;
#else
    config->i2s_dataout_pin = 25;
#endif

    // TI PCM5102A mute pin (XMT), -1 disables mute control
#ifdef CONFIG_PCM5102A_MUTE_PIN
    config->pcm5102a_mute_pin = CONFIG_PCM5102A_MUTE_PIN;
#else
    config->pcm5102a_mute_pin = 26;
#endif

    config->ap_mode_button_gpio = 19;
    config->stop_wifi_during_bt = true;

    // Audio source toggles
    config->snapcast_audio_enabled = true;
    config->bluetooth_audio_enabled = true;

    // Dynamic bass mapping defaults
    config->bass_mapping_enabled = false;
    config->bass_mapping_low_gain = 0.0f;
    config->bass_mapping_high_gain = 0.0f;
    strncpy(config->bass_mapping_curve, "linear", sizeof(config->bass_mapping_curve) - 1);
    config->bass_mapping_curve[sizeof(config->bass_mapping_curve) - 1] = '\0';

    // Dynamic mids mapping defaults
    config->mids_mapping_enabled = false;
    config->mids_mapping_low_gain = 0.0f;
    config->mids_mapping_high_gain = 0.0f;
    strncpy(config->mids_mapping_curve, "linear", sizeof(config->mids_mapping_curve) - 1);
    config->mids_mapping_curve[sizeof(config->mids_mapping_curve) - 1] = '\0';

    // Dynamic treble mapping defaults
    config->treble_mapping_enabled = false;
    config->treble_mapping_low_gain = 0.0f;
    config->treble_mapping_high_gain = 0.0f;
    strncpy(config->treble_mapping_curve, "linear", sizeof(config->treble_mapping_curve) - 1);
    config->treble_mapping_curve[sizeof(config->treble_mapping_curve) - 1] = '\0';
}

esp_err_t system_config_load_from_nvs(system_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    // Start from compile-time defaults
    system_config_set_defaults(config);

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_SYS, NVS_READONLY, &nvs_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No system config in NVS, using defaults");
        return ESP_ERR_NOT_FOUND;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t u8;
    if (nvs_get_u8(nvs_handle, "vol_btn_en", &u8) == ESP_OK) {
        config->volume_buttons_enabled = (u8 != 0);
    }

    int32_t i32;
    if (nvs_get_i32(nvs_handle, "vol_up_pin", &i32) == ESP_OK) {
        config->volume_up_pin = (int)i32;
    }
    if (nvs_get_i32(nvs_handle, "vol_dn_pin", &i32) == ESP_OK) {
        config->volume_down_pin = (int)i32;
    }

    if (nvs_get_u8(nvs_handle, "eff_btn_en", &u8) == ESP_OK) {
        config->effect_button_enabled = (u8 != 0);
    }

    if (nvs_get_i32(nvs_handle, "eff_btn_pin", &i32) == ESP_OK) {
        config->effect_button_pin = (int)i32;
    }

    if (nvs_get_u8(nvs_handle, "snapcast_en", &u8) == ESP_OK) {
        config->snapcast_audio_enabled = (u8 != 0);
    }

    if (nvs_get_u8(nvs_handle, "bluetooth_en", &u8) == ESP_OK) {
        config->bluetooth_audio_enabled = (u8 != 0);
    }

    if (nvs_get_u8(nvs_handle, "stop_wifi_bt", &u8) == ESP_OK) {
        config->stop_wifi_during_bt = (u8 != 0);
    }

    size_t len = SYSTEM_CONFIG_MAX_NAME_LEN;
    if (nvs_get_str(nvs_handle, "name", config->snapclient_name, &len) != ESP_OK) {
        // keep default name
    }

    char gain_str[16];
    len = sizeof(gain_str);
    if (nvs_get_str(nvs_handle, "gain", gain_str, &len) == ESP_OK) {
        float g = (float)atof(gain_str);
        if (g > 0.0f && g < 10.0f) {
            config->snapcast_gain_boost = g;
        }
    }

    len = SYSTEM_CONFIG_MAX_SSID_LEN;
    if (nvs_get_str(nvs_handle, "wifi_ssid", config->wifi_ssid, &len) != ESP_OK) {
        // keep default SSID
    }

    len = SYSTEM_CONFIG_MAX_PASS_LEN;
    if (nvs_get_str(nvs_handle, "wifi_pwd", config->wifi_password, &len) != ESP_OK) {
        // keep default password
    }

    len = SYSTEM_CONFIG_MAX_HOST_LEN;
    if (nvs_get_str(nvs_handle, "srv_host", config->snapserver_host, &len) != ESP_OK) {
        // keep default host
    }

    if (nvs_get_i32(nvs_handle, "srv_port", &i32) == ESP_OK && i32 > 0 && i32 <= 65535) {
        config->snapserver_port = (int)i32;
    }

    if (nvs_get_u8(nvs_handle, "sh1106_en", &u8) == ESP_OK) {
        config->sh1106_enabled = (u8 != 0);
    }

    if (nvs_get_i32(nvs_handle, "sh1106_sda", &i32) == ESP_OK) {
        config->sh1106_sda_gpio = (int)i32;
    }

    if (nvs_get_i32(nvs_handle, "sh1106_scl", &i32) == ESP_OK) {
        config->sh1106_scl_gpio = (int)i32;
    }

    if (nvs_get_i32(nvs_handle, "sh1106_freq", &i32) == ESP_OK && i32 > 0 && i32 <= 1000000) {
        config->sh1106_i2c_freq_hz = (int)i32;
    }

    if (nvs_get_i32(nvs_handle, "sh1106_col", &i32) == ESP_OK && i32 >= 0 && i32 <= 127) {
        config->sh1106_column_offset = (int)i32;
    }

    // Optional overrides for audio pins; basic range check  -1..39
    if (nvs_get_i32(nvs_handle, "i2s_mclk", &i32) == ESP_OK && i32 >= -1 && i32 <= 39) {
        config->i2s_mclk_pin = (int)i32;
    }

    if (nvs_get_i32(nvs_handle, "i2s_bck", &i32) == ESP_OK && i32 >= -1 && i32 <= 39) {
        config->i2s_bck_pin = (int)i32;
    }

    if (nvs_get_i32(nvs_handle, "i2s_lrck", &i32) == ESP_OK && i32 >= -1 && i32 <= 39) {
        config->i2s_lrck_pin = (int)i32;
    }

    if (nvs_get_i32(nvs_handle, "i2s_data", &i32) == ESP_OK && i32 >= -1 && i32 <= 39) {
        config->i2s_dataout_pin = (int)i32;
    }

    if (nvs_get_i32(nvs_handle, "pcm_mute", &i32) == ESP_OK && i32 >= -1 && i32 <= 39) {
        config->pcm5102a_mute_pin = (int)i32;
    }

    if (nvs_get_i32(nvs_handle, "ap_btn_gpio", &i32) == ESP_OK && i32 >= 0 && i32 <= 48) {
        config->ap_mode_button_gpio = (int)i32;
    }

    // Load bass mapping settings
    if (nvs_get_u8(nvs_handle, "bass_map_en", &u8) == ESP_OK) {
        config->bass_mapping_enabled = (u8 != 0);
    }

    char bass_str[16];
    len = sizeof(bass_str);
    if (nvs_get_str(nvs_handle, "bass_low", bass_str, &len) == ESP_OK) {
        config->bass_mapping_low_gain = (float)atof(bass_str);
    }

    len = sizeof(bass_str);
    if (nvs_get_str(nvs_handle, "bass_high", bass_str, &len) == ESP_OK) {
        config->bass_mapping_high_gain = (float)atof(bass_str);
    }

    len = sizeof(config->bass_mapping_curve);
    if (nvs_get_str(nvs_handle, "bass_curve", config->bass_mapping_curve, &len) != ESP_OK) {
        // keep default curve
    }

    // Load mids mapping settings
    if (nvs_get_u8(nvs_handle, "mids_map_en", &u8) == ESP_OK) {
        config->mids_mapping_enabled = (u8 != 0);
    }

    char mids_str[16];
    len = sizeof(mids_str);
    if (nvs_get_str(nvs_handle, "mids_low", mids_str, &len) == ESP_OK) {
        config->mids_mapping_low_gain = (float)atof(mids_str);
    }

    len = sizeof(mids_str);
    if (nvs_get_str(nvs_handle, "mids_high", mids_str, &len) == ESP_OK) {
        config->mids_mapping_high_gain = (float)atof(mids_str);
    }

    len = sizeof(config->mids_mapping_curve);
    if (nvs_get_str(nvs_handle, "mids_curve", config->mids_mapping_curve, &len) != ESP_OK) {
        // keep default curve
    }

    // Load treble mapping settings
    if (nvs_get_u8(nvs_handle, "treb_map_en", &u8) == ESP_OK) {
        config->treble_mapping_enabled = (u8 != 0);
    }

    char treb_str[16];
    len = sizeof(treb_str);
    if (nvs_get_str(nvs_handle, "treb_low", treb_str, &len) == ESP_OK) {
        config->treble_mapping_low_gain = (float)atof(treb_str);
    }

    len = sizeof(treb_str);
    if (nvs_get_str(nvs_handle, "treb_high", treb_str, &len) == ESP_OK) {
        config->treble_mapping_high_gain = (float)atof(treb_str);
    }

    len = sizeof(config->treble_mapping_curve);
    if (nvs_get_str(nvs_handle, "treb_curve", config->treble_mapping_curve, &len) != ESP_OK) {
        // keep default curve
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Loaded system config from NVS: name='%s', vol_up=%d, vol_down=%d, gain=%.2f, buttons=%s",
             config->snapclient_name,
             config->volume_up_pin,
             config->volume_down_pin,
             config->snapcast_gain_boost,
             config->volume_buttons_enabled ? "on" : "off");
    return ESP_OK;
}

esp_err_t system_config_save_to_nvs(const system_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_SYS, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(nvs_handle, "vol_btn_en", config->volume_buttons_enabled ? 1 : 0);
    if (err != ESP_OK) goto out;

    err = nvs_set_i32(nvs_handle, "vol_up_pin", (int32_t)config->volume_up_pin);
    if (err != ESP_OK) goto out;

    err = nvs_set_i32(nvs_handle, "vol_dn_pin", (int32_t)config->volume_down_pin);
    if (err != ESP_OK) goto out;

    err = nvs_set_u8(nvs_handle, "eff_btn_en", config->effect_button_enabled ? 1 : 0);
    if (err != ESP_OK) goto out;

    err = nvs_set_i32(nvs_handle, "eff_btn_pin", (int32_t)config->effect_button_pin);
    if (err != ESP_OK) goto out;

    err = nvs_set_u8(nvs_handle, "snapcast_en", config->snapcast_audio_enabled ? 1 : 0);
    if (err != ESP_OK) goto out;

    err = nvs_set_u8(nvs_handle, "bluetooth_en", config->bluetooth_audio_enabled ? 1 : 0);
    if (err != ESP_OK) goto out;

    err = nvs_set_u8(nvs_handle, "stop_wifi_bt", config->stop_wifi_during_bt ? 1 : 0);
    if (err != ESP_OK) goto out;

    err = nvs_set_str(nvs_handle, "name", config->snapclient_name);
    if (err != ESP_OK) goto out;

    char gain_str[16];
    snprintf(gain_str, sizeof(gain_str), "%.3f", (double)config->snapcast_gain_boost);
    err = nvs_set_str(nvs_handle, "gain", gain_str);
    if (err != ESP_OK) goto out;

    err = nvs_set_str(nvs_handle, "wifi_ssid", config->wifi_ssid);
    if (err != ESP_OK) goto out;

    err = nvs_set_str(nvs_handle, "wifi_pwd", config->wifi_password);
    if (err != ESP_OK) goto out;

    err = nvs_set_str(nvs_handle, "srv_host", config->snapserver_host);
    if (err != ESP_OK) goto out;

    err = nvs_set_i32(nvs_handle, "srv_port", (int32_t)config->snapserver_port);
    if (err != ESP_OK) goto out;

    err = nvs_set_u8(nvs_handle, "sh1106_en", config->sh1106_enabled ? 1 : 0);
    if (err != ESP_OK) goto out;

    err = nvs_set_i32(nvs_handle, "sh1106_sda", (int32_t)config->sh1106_sda_gpio);
    if (err != ESP_OK) goto out;

    err = nvs_set_i32(nvs_handle, "sh1106_scl", (int32_t)config->sh1106_scl_gpio);
    if (err != ESP_OK) goto out;

    err = nvs_set_i32(nvs_handle, "sh1106_freq", (int32_t)config->sh1106_i2c_freq_hz);
    if (err != ESP_OK) goto out;

    err = nvs_set_i32(nvs_handle, "sh1106_col", (int32_t)config->sh1106_column_offset);
    if (err != ESP_OK) goto out;

    // Persist audio pin overrides
    err = nvs_set_i32(nvs_handle, "i2s_mclk", (int32_t)config->i2s_mclk_pin);
    if (err != ESP_OK) goto out;

    err = nvs_set_i32(nvs_handle, "i2s_bck", (int32_t)config->i2s_bck_pin);
    if (err != ESP_OK) goto out;

    err = nvs_set_i32(nvs_handle, "i2s_lrck", (int32_t)config->i2s_lrck_pin);
    if (err != ESP_OK) goto out;

    err = nvs_set_i32(nvs_handle, "i2s_data", (int32_t)config->i2s_dataout_pin);
    if (err != ESP_OK) goto out;

    err = nvs_set_i32(nvs_handle, "pcm_mute", (int32_t)config->pcm5102a_mute_pin);
    if (err != ESP_OK) goto out;

    err = nvs_set_i32(nvs_handle, "ap_btn_gpio", (int32_t)config->ap_mode_button_gpio);
    if (err != ESP_OK) goto out;

    // Save bass mapping settings
    err = nvs_set_u8(nvs_handle, "bass_map_en", config->bass_mapping_enabled ? 1 : 0);
    if (err != ESP_OK) goto out;

    char bass_str[16];
    snprintf(bass_str, sizeof(bass_str), "%.1f", (double)config->bass_mapping_low_gain);
    err = nvs_set_str(nvs_handle, "bass_low", bass_str);
    if (err != ESP_OK) goto out;

    snprintf(bass_str, sizeof(bass_str), "%.1f", (double)config->bass_mapping_high_gain);
    err = nvs_set_str(nvs_handle, "bass_high", bass_str);
    if (err != ESP_OK) goto out;

    err = nvs_set_str(nvs_handle, "bass_curve", config->bass_mapping_curve);
    if (err != ESP_OK) goto out;

    // Save mids mapping settings
    err = nvs_set_u8(nvs_handle, "mids_map_en", config->mids_mapping_enabled ? 1 : 0);
    if (err != ESP_OK) goto out;

    char mids_str[16];
    snprintf(mids_str, sizeof(mids_str), "%.1f", (double)config->mids_mapping_low_gain);
    err = nvs_set_str(nvs_handle, "mids_low", mids_str);
    if (err != ESP_OK) goto out;

    snprintf(mids_str, sizeof(mids_str), "%.1f", (double)config->mids_mapping_high_gain);
    err = nvs_set_str(nvs_handle, "mids_high", mids_str);
    if (err != ESP_OK) goto out;

    err = nvs_set_str(nvs_handle, "mids_curve", config->mids_mapping_curve);
    if (err != ESP_OK) goto out;

    // Save treble mapping settings
    err = nvs_set_u8(nvs_handle, "treb_map_en", config->treble_mapping_enabled ? 1 : 0);
    if (err != ESP_OK) goto out;

    char treb_str[16];
    snprintf(treb_str, sizeof(treb_str), "%.1f", (double)config->treble_mapping_low_gain);
    err = nvs_set_str(nvs_handle, "treb_low", treb_str);
    if (err != ESP_OK) goto out;

    snprintf(treb_str, sizeof(treb_str), "%.1f", (double)config->treble_mapping_high_gain);
    err = nvs_set_str(nvs_handle, "treb_high", treb_str);
    if (err != ESP_OK) goto out;

    err = nvs_set_str(nvs_handle, "treb_curve", config->treble_mapping_curve);
    if (err != ESP_OK) goto out;

    err = nvs_commit(nvs_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "System config saved to NVS");
    } else {
        ESP_LOGE(TAG, "Failed to commit system config: %s", esp_err_to_name(err));
    }

out:
    nvs_close(nvs_handle);
    return err;
}
