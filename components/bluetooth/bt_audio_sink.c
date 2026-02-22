#include "bt_audio_sink.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "freertos/semphr.h"
#include <math.h> 
#include "player.h"
#include "bt_audio_task.h"
#include "system_config.h"
#include "esp_wifi.h"
#include "esp_coexist.h"

#ifdef CONFIG_ENABLE_LED_CONTROLLER
#include "led_controller.h"
#endif

#if CONFIG_ENABLE_SH1106_DISPLAY
#include "display_sh1106.h"
#endif

#define TAG "BT_AUDIO_SINK"

// Transaction labels for AVRC commands
#define APP_RC_CT_TL_GET_CAPS            (0)
#define APP_RC_CT_TL_GET_META_DATA       (1)
#define APP_RC_CT_TL_RN_TRACK_CHANGE     (2)

static bool bt_connected = false;
static bool avrc_connected = false;
static char bt_remote_device_name[64] = "Unknown Device";
static uint8_t s_volume = 0x7F; // Max volume by default
static int local_bt_volume = 50; // Shared local Bluetooth volume state
static uint8_t s_eq_levels[16] = {0};
static uint32_t s_last_eq_update = 0;
static esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;
static bool s_volume_notify = false; // Volume notification state

// Bluetooth metadata storage
static char bt_current_title[128] = "";
static char bt_current_artist[128] = "";
static char bt_current_album[64] = "";
static char bt_current_genre[32] = "";
static bool metadata_updated = false;

static void bt_av_new_track(void);

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
static void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len);
static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
static void bt_app_rc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
static void volume_set_by_local_host(uint8_t volume);

extern void audio_set_mute(bool mute);

void bt_audio_sink_init() {
    ESP_LOGI(TAG, "Initializing Bluetooth A2DP sink");

    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_LOGI(TAG, "BT Controller status before init: %d", esp_bt_controller_get_status());

    esp_err_t err;
    if ((err = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "%s initialize controller failed: %s\n", __func__, esp_err_to_name(err));
        return;
    }

    if ((err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        ESP_LOGE(TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name(err));
        return;
    }

    if ((err = esp_bluedroid_init()) != ESP_OK) {
        ESP_LOGE(TAG, "%s initialize bluedroid failed: %s\n", __func__, esp_err_to_name(err));
        return;
    }

    if ((err = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(TAG, "%s enable bluedroid failed: %s\n", __func__, esp_err_to_name(err));
        return;
    }

#if (CONFIG_BT_SSP_ENABLED)
    /* Set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif

    // Initialize AVRC FIRST (critical for proper service discovery)
    esp_err_t ret = esp_avrc_ct_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AVRC CT init failed: %s", esp_err_to_name(ret));
        return;
    }
    esp_avrc_ct_register_callback(bt_app_rc_ct_cb);
    
    ret = esp_avrc_tg_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AVRC TG init failed: %s", esp_err_to_name(ret));
        return;
    }
    esp_avrc_tg_register_callback(bt_app_rc_tg_cb);
    
    // Enable volume change notifications (critical for A2DP-style volume sync)
    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    ret = esp_avrc_tg_set_rn_evt_cap(&evt_set);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "AVRC volume change notifications enabled");
    } else {
        ESP_LOGE(TAG, "Failed to enable AVRC volume notifications: %s", esp_err_to_name(ret));
        return;
    }

    // Initialize A2DP after AVRC for proper service discovery
    ret = esp_a2d_sink_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP sink init failed: %s", esp_err_to_name(ret));
        return;
    }
    esp_a2d_register_callback(&bt_app_a2d_cb);
    esp_a2d_sink_register_data_callback(bt_app_a2d_data_cb);

    /* Get the default value of the delay value */
    esp_a2d_sink_get_delay_value();

    // Use runtime-configured snapclient name for Bluetooth device name
    system_config_t cfg;
    system_config_set_defaults(&cfg);
    system_config_load_from_nvs(&cfg);
    esp_bt_dev_set_device_name(cfg.snapclient_name);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_bt_gap_register_callback(&bt_app_gap_cb);
}

bool bt_audio_sink_is_connected() {
    // Only log connection status on state changes or errors
    static bool last_a2dp_state = false;
    static bool last_avrc_state = false;
    static uint32_t last_status_log = 0;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Log on state changes
    if (bt_connected != last_a2dp_state || avrc_connected != last_avrc_state) {
        ESP_LOGI(TAG, "BT status change: A2DP=%s, AVRC=%s", 
                 bt_connected ? "CONNECTED" : "DISCONNECTED",
                 avrc_connected ? "CONNECTED" : "DISCONNECTED");
        last_a2dp_state = bt_connected;
        last_avrc_state = avrc_connected;
        last_status_log = now;
    }
    // Occasional heartbeat only if there are connection issues (every 30 seconds)
    else if (bt_connected && !avrc_connected && now - last_status_log > 30000) {
        ESP_LOGW(TAG, "A2DP connected but AVRC still disconnected after 30s");
        last_status_log = now;
    }
    
    return bt_connected;
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_BT_GAP_PIN_REQ_EVT: {
            ESP_LOGI(TAG, "PIN code requested");

            // Respond with your chosen PIN
            esp_bt_pin_code_t pin_code;
            strcpy((char *)pin_code, "0000");  // 4-digit PIN (default)
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
            break;
        }
        case ESP_BT_GAP_AUTH_CMPL_EVT: {
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Authentication success: %s", param->auth_cmpl.device_name);
                // Store device name
                strncpy(bt_remote_device_name, (char *)param->auth_cmpl.device_name, sizeof(bt_remote_device_name) - 1);
                bt_remote_device_name[sizeof(bt_remote_device_name) - 1] = '\0';
#if CONFIG_ENABLE_SH1106_DISPLAY
                display_set_bt_device_name(bt_remote_device_name);
#endif
            } else {
                ESP_LOGE(TAG, "Authentication failed, status: %d", param->auth_cmpl.stat);
            }
            break;
        }
        default:
            break;
    }
}

static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                ESP_LOGI(TAG, "A2DP connected - AVRC will connect automatically if supported");
                bt_connected = true;
                
                // Initialize volume to a reasonable default (50%)
                local_bt_volume = 50;
                s_volume = (50 * 127) / 100; // Convert to abs_vol
                ESP_LOGI(TAG, "Initialized Bluetooth volume to 50%%");
                
#if CONFIG_ENABLE_SH1106_DISPLAY
                display_set_bt_signal(true, 0);  // Connected with default signal
                // Ensure the display shows the source BT device name when connected
                display_set_bt_device_name(bt_remote_device_name);
                display_set_audio_levels(s_eq_levels, 50, false); // Show initial volume
#endif
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                ESP_LOGI(TAG, "Bluetooth disconnected");
                bt_connected = false;
                avrc_connected = false;
#if CONFIG_ENABLE_SH1106_DISPLAY
                display_set_bt_signal(false, -100);  // Disconnected
                display_set_bt_device_name("");  // Clear device name
#endif
            }
            break;
        case ESP_A2D_AUDIO_STATE_EVT:
            if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                ESP_LOGI(TAG, "Bluetooth started playing, setting up override");
                override_player();
                // Mute Snapcast when Bluetooth audio starts
                snapcast_mute_for_bluetooth();
                // 🔽 Give Bluetooth priority over WiFi
                esp_coex_preference_set(ESP_COEX_PREFER_BT);
                // 🔽 Reduce WiFi RF interference
                esp_wifi_set_max_tx_power(40);  // try 40–52 first
                esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
#if CONFIG_ENABLE_SH1106_DISPLAY
                // Initialize with placeholder metadata
                strncpy(bt_current_title, "Loading...", sizeof(bt_current_title) - 1);
                strncpy(bt_current_artist, "Connecting...", sizeof(bt_current_artist) - 1);
                display_set_song_metadata(bt_current_title, bt_current_artist, "", "");
                display_set_bt_audio_playing(true);
                // Request metadata immediately if AVRC is connected
                if (avrc_connected) {
                    bt_av_new_track();
                }
#endif
            } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND ||
                       param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED) {
                ESP_LOGI(TAG, "Bluetooth stopped playing, giving back control to Snapcast");
                // Clear queue first
                pcm_chunk_message_t *chunk = NULL;
                while (xQueueReceive(bluetooth_pcm_queue, &chunk, 0) == pdTRUE) {}
                deoverride_player();
                // Unmute Snapcast when Bluetooth audio stops
                snapcast_unmute_after_bluetooth();
                // 🔽 Restore WiFi priority
                esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
                // 🔽 Restore WiFi power
                esp_wifi_set_max_tx_power(78);
                esp_wifi_set_ps(WIFI_PS_NONE);
#if CONFIG_ENABLE_SH1106_DISPLAY
                display_set_bt_audio_playing(false);
                // Clear Bluetooth metadata
                memset(bt_current_title, 0, sizeof(bt_current_title));
                memset(bt_current_artist, 0, sizeof(bt_current_artist));
                memset(bt_current_album, 0, sizeof(bt_current_album));
                memset(bt_current_genre, 0, sizeof(bt_current_genre));
                metadata_updated = false;
                // Restore snapclient metadata
                display_set_song_metadata("Snapclient", "Ready", "", "");
#endif
            }
        default:
            break;
    }
}

static void bt_av_new_track(void)
{
    // Request metadata with all attributes at once
    uint8_t attr_mask = ESP_AVRC_MD_ATTR_TITLE |
                        ESP_AVRC_MD_ATTR_ARTIST |
                        ESP_AVRC_MD_ATTR_ALBUM |
                        ESP_AVRC_MD_ATTR_GENRE;
    esp_avrc_ct_send_metadata_cmd(APP_RC_CT_TL_GET_META_DATA, attr_mask);
    ESP_LOGI(TAG, "Requesting metadata");

    // Register notification for track changes if peer supports it
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap,
                                           ESP_AVRC_RN_TRACK_CHANGE)) {
        esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_TRACK_CHANGE,
                                                   ESP_AVRC_RN_TRACK_CHANGE, 0);
    }
}

static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    
    switch (event) {
        case ESP_AVRC_CT_METADATA_RSP_EVT: {
#if CONFIG_ENABLE_SH1106_DISPLAY
            ESP_LOGI(TAG, "Metadata response: attr_id=%d, attr_text=%s", 
                     param->meta_rsp.attr_id, param->meta_rsp.attr_text);
            
            uint8_t *attr_text = param->meta_rsp.attr_text;
            
            // Store metadata in local variables
            if (param->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_TITLE && attr_text) {
                strncpy(bt_current_title, (char *)attr_text, sizeof(bt_current_title) - 1);
                bt_current_title[sizeof(bt_current_title) - 1] = '\0';
                metadata_updated = true;
            } else if (param->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_ARTIST && attr_text) {
                strncpy(bt_current_artist, (char *)attr_text, sizeof(bt_current_artist) - 1);
                bt_current_artist[sizeof(bt_current_artist) - 1] = '\0';
                metadata_updated = true;
            } else if (param->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_ALBUM && attr_text) {
                strncpy(bt_current_album, (char *)attr_text, sizeof(bt_current_album) - 1);
                bt_current_album[sizeof(bt_current_album) - 1] = '\0';
                metadata_updated = true;
            } else if (param->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_GENRE && attr_text) {
                strncpy(bt_current_genre, (char *)attr_text, sizeof(bt_current_genre) - 1);
                bt_current_genre[sizeof(bt_current_genre) - 1] = '\0';
                metadata_updated = true;
            }
            
            // Update display with current metadata (preserving what we have)
            if (metadata_updated) {
                const char *title = strlen(bt_current_title) > 0 ? bt_current_title : "Unknown Track";
                const char *artist = strlen(bt_current_artist) > 0 ? bt_current_artist : "Unknown Artist";
                display_set_song_metadata(title, artist, bt_current_album, bt_current_genre);
                ESP_LOGI(TAG, "Updated metadata: Title='%s', Artist='%s'", title, artist);
            }
#endif
            break;
        }
        case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
            if (param->conn_stat.connected) {
                ESP_LOGI(TAG, "AVRC CT connected");
                avrc_connected = true;
                
                // Enable volume change notifications
                esp_avrc_ct_send_register_notification_cmd(1, ESP_AVRC_RN_VOLUME_CHANGE, 0);
                
                // Get remote capabilities
                esp_avrc_ct_send_get_rn_capabilities_cmd(APP_RC_CT_TL_GET_CAPS);
                
                // Try to sync current volume
                ESP_LOGI(TAG, "Syncing current volume: %d%%", local_bt_volume);
                bt_audio_set_absolute_volume_percent(local_bt_volume);
                
                // Request metadata if audio is already playing
                if (bt_connected) {
                    ESP_LOGI(TAG, "AVRC connected while audio playing - requesting metadata");
                    bt_av_new_track();
                }
                
            } else {
                ESP_LOGI(TAG, "AVRC CT disconnected");
                avrc_connected = false;
            }
            break;
        }
        case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
            ESP_LOGI(TAG, "Remote notification capabilities: count %d, bitmask 0x%x", 
                     param->get_rn_caps_rsp.cap_count, param->get_rn_caps_rsp.evt_set.bits);
            s_avrc_peer_rn_cap.bits = param->get_rn_caps_rsp.evt_set.bits;
            // Now request metadata
            bt_av_new_track();
            break;
        }
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
            if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
                ESP_LOGI(TAG, "Track change notification - requesting metadata");
                // Clear current metadata to force refresh
                memset(bt_current_title, 0, sizeof(bt_current_title));
                memset(bt_current_artist, 0, sizeof(bt_current_artist));
                bt_av_new_track();
            }
            break;
        }
        default:
            break;
    }
}

// Function to handle phone volume changes
static void volume_set_by_controller(uint8_t volume) {
    // Ignore if volume is unchanged to avoid flooding updates
    if (volume == s_volume) {
        return;
    }

    ESP_LOGI(TAG, "Phone changed volume to: %d (internal sync)", volume);
    
    // Update local state to match phone
    s_volume = volume;
    local_bt_volume = (volume * 100) / 127;
    
    // Update display to show new volume
#if CONFIG_ENABLE_SH1106_DISPLAY
    display_set_audio_levels(s_eq_levels, local_bt_volume, true);
#endif
}

static void bt_app_rc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    
    switch (event) {
        case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
            uint8_t volume = param->set_abs_vol.volume;
            int volume_percent = (volume * 100) / 127;
            
            ESP_LOGI(TAG, "Volume set by remote device: %d%% (abs_vol=%d)", volume_percent, volume);
            
            // Sync local state with remote device
            volume_set_by_controller(volume);
            
            ESP_LOGI(TAG, "Synchronized local volume state to match remote: %d%%", volume_percent);
            break;
        }
        case ESP_AVRC_TG_REMOTE_FEATURES_EVT: {
            ESP_LOGI(TAG, "AVRC remote features: 0x%" PRIx32, param->rmt_feats.feat_mask);
            break;
        }
        case ESP_AVRC_TG_CONNECTION_STATE_EVT: {
            ESP_LOGI(TAG, "AVRC TG connection state: %s", 
                     param->conn_stat.connected ? "CONNECTED" : "DISCONNECTED");
            if (param->conn_stat.connected) {
                avrc_connected = true;
            } else {
                avrc_connected = false;
            }
            break;
        }
        case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT: {
            if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
                s_volume_notify = true;
                esp_avrc_rn_param_t rn_param;
                rn_param.volume = s_volume;
                esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
                ESP_LOGI(TAG, "Volume notifications enabled");
            }
            break;
        }
        default:
            break;
    }
}

void bt_audio_set_absolute_volume_percent(int percent) {
    if (!bt_connected) {
        ESP_LOGW(TAG, "Absolute volume %d%% ignored - A2DP not connected", percent);
        return;
    }
    
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint8_t abs_vol = (uint8_t)((percent * 127) / 100);
    
    ESP_LOGI(TAG, "Setting volume: %d%%", percent);
    
    // Update local volume and send notifications
    volume_set_by_local_host(abs_vol);
    
    // Also send absolute volume command if AVRC is connected
    if (avrc_connected) {
        esp_avrc_ct_send_set_absolute_volume_cmd(0, abs_vol);
    }
}

// Function to notify phone of local volume changes (from A2DP firmware)
static void volume_set_by_local_host(uint8_t volume) {
    // Ignore if volume is unchanged to avoid redundant work
    if (volume == s_volume) {
        return;
    }

    // Update local state first
    s_volume = volume;
    local_bt_volume = (volume * 100) / 127;
    
    // Update display with new volume
#if CONFIG_ENABLE_SH1106_DISPLAY
    display_set_audio_levels(s_eq_levels, local_bt_volume, true);
#endif

    // Send notification response to remote AVRCP controller (if registered)
    if (s_volume_notify && avrc_connected) {
        esp_avrc_rn_param_t rn_param;
        rn_param.volume = s_volume;
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn_param);
        s_volume_notify = false; // Reset after notification
    }
}

void bt_audio_volume_up() {
    if (!bt_connected) {
        ESP_LOGW(TAG, "Volume up ignored - A2DP not connected");
        return;
    }
    
    // Calculate new volume using A2DP firmware logic (8-level increments)
    uint8_t new_abs_vol;
    if (s_volume <= 119) {
        new_abs_vol = s_volume + 8; // ~6% increment
    } else {
        new_abs_vol = 127; // Max volume
    }
    
    int new_percent = (new_abs_vol * 100) / 127;
    
    ESP_LOGI(TAG, "Volume: %d%% -> %d%%", local_bt_volume, new_percent);
    
    // Update local volume and notify phone
    volume_set_by_local_host(new_abs_vol);
    
    // Update display
#if CONFIG_ENABLE_SH1106_DISPLAY
    display_set_audio_levels(s_eq_levels, new_percent, true);
#endif
}

void bt_audio_volume_down() {
    if (!bt_connected) {
        ESP_LOGW(TAG, "Volume down ignored - A2DP not connected");
        return;
    }
    
    // Calculate new volume using A2DP firmware logic (8-level decrements)
    uint8_t new_abs_vol;
    if (s_volume >= 8) {
        new_abs_vol = s_volume - 8; // ~6% decrement
    } else {
        new_abs_vol = 0; // Min volume (mute)
    }
    
    int new_percent = (new_abs_vol * 100) / 127;
    
    ESP_LOGI(TAG, "Volume: %d%% -> %d%%", local_bt_volume, new_percent);
    
    // Update local volume and notify phone
    volume_set_by_local_host(new_abs_vol);
    
    // Update display
#if CONFIG_ENABLE_SH1106_DISPLAY
    display_set_audio_levels(s_eq_levels, new_percent, true);
#endif
}

int bt_audio_get_volume_percent() {
    return local_bt_volume;
}

static void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len) {
    pcm_chunk_message_t *pcmChunk = NULL;

    if (allocate_pcm_chunk_memory(&pcmChunk, len) < 0) {
        ESP_LOGE(TAG, "Failed to allocate PCM chunk");
        return;
    }

    // Apply volume scaling to audio data
    if (len >= 2 && s_volume < 127) {
        int16_t *samples = (int16_t *)data;
        int16_t *output_samples = (int16_t *)pcmChunk->fragment->payload;
        uint32_t sample_count = len / 2;

        // Non-linear volume curve using powf()
        float norm = (float)s_volume / 127.0f;
        float processed_volume = powf(norm, 2.5f);

        for (uint32_t i = 0; i < sample_count; i++) {
            int32_t scaled_sample = (int32_t)(samples[i] * processed_volume);
            // Clamp to prevent overflow
            if (scaled_sample > 32767) scaled_sample = 32767;
            if (scaled_sample < -32768) scaled_sample = -32768;
            output_samples[i] = (int16_t)scaled_sample;
        }
    } else {
        // Full volume or invalid data - just copy
        memcpy(pcmChunk->fragment->payload, data, len);
    }

        pcmChunk->totalSize = len;
        pcmChunk->fragment->size = len;
        pcmChunk->fragment->nextFragment = NULL;

#if CONFIG_ENABLE_SH1106_DISPLAY
    // Calculate EQ levels from original audio data (before volume scaling)
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Periodic metadata refresh every 30 seconds if no metadata or showing placeholder
    static uint32_t last_metadata_request = 0;
    if (avrc_connected && (current_time - last_metadata_request >= 30000)) {
        if (strlen(bt_current_title) == 0 || 
            strcmp(bt_current_title, "Loading...") == 0 || 
            strcmp(bt_current_title, "Unknown Track") == 0) {
            ESP_LOGI(TAG, "Periodic metadata refresh - no valid metadata");
            bt_av_new_track();
            last_metadata_request = current_time;
        }
    }
    
    if (current_time - s_last_eq_update >= 100 && len >= 2) { // Update every 100ms
        s_last_eq_update = current_time;
        
        // Use original audio data for EQ calculation (before volume scaling)
        int16_t *samples = (int16_t *)data;
        uint32_t sample_count = len / 2;
        
        // Calculate global energy for unified effect
        int32_t total_energy = 0;
        for (uint32_t i = 0; i < sample_count; i++) {
            total_energy += abs(samples[i]);
        }
        int global_energy_boost = (total_energy / sample_count) / 800;
        if (global_energy_boost > 60) global_energy_boost = 60;
        if (global_energy_boost < 0) global_energy_boost = 0;
        
        // Simple approach: divide audio into 16 bands and analyze peak energy
        for (int band = 0; band < 16; band++) {
            int32_t max_amplitude = 0;
            
            // Each band analyzes a portion of the audio samples
            uint32_t start = (band * sample_count) / 16;
            uint32_t end = ((band + 1) * sample_count) / 16;
            
            // Find peak amplitude in this band section
            for (uint32_t i = start; i < end; i++) {
                int32_t abs_sample = abs(samples[i]);
                if (abs_sample > max_amplitude) {
                    max_amplitude = abs_sample;
                }
            }
            
            // Convert to display height (0-100) - aggressive scaling
            int bar_height = 0;
            if (max_amplitude > 100) { // Low noise threshold
                if (band <= 3) {
                    // Bass bars: High sensitivity
                    bar_height = (max_amplitude * 300) / 32768;
                } else if (band <= 11) {
                    // Mid bars: Medium sensitivity  
                    bar_height = (max_amplitude * 250) / 32768;
                } else {
                    // Treble bars: Lower sensitivity
                    bar_height = (max_amplitude * 220) / 32768;
                }
                
                // Apply global energy boost
                bar_height += global_energy_boost;
                
                if (bar_height > 100) bar_height = 100;
                if (bar_height < 8) bar_height = 8;
            } else {
                bar_height = 2; // Minimal baseline
            }
            
            // Simple smoothing
            static int prev_bar_heights[16] = {8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8};
            bar_height = (prev_bar_heights[band] * 3 + bar_height * 7) / 10; // 70% new, 30% old
            prev_bar_heights[band] = bar_height;
            
            s_eq_levels[band] = (uint8_t)bar_height;
        }
        
        display_set_audio_levels(s_eq_levels, (s_volume * 100) / 127, true);
    }
#endif

    // Feed Bluetooth PCM into LED controller.
    // When "Ignore Playback Volume" is enabled in the LED config,
    // use the original (pre-volume) A2DP data so LED sensitivity
    // is independent of Bluetooth volume. Otherwise, use the
    // scaled buffer so LEDs follow playback volume as before.
#ifdef CONFIG_ENABLE_LED_CONTROLLER
    led_config_t led_cfg;
    bool ignore_volume_for_led = false;
    if (led_controller_get_config(&led_cfg) == ESP_OK) {
        ignore_volume_for_led = (led_cfg.ignore_volume != 0);
    }

    if (ignore_volume_for_led) {
        if (len >= 4) {
            const int16_t *samples = (const int16_t *)data;  // pre-volume
            size_t num_samples_per_channel = len / 4;        // stereo int16
            led_controller_feed_audio(samples, num_samples_per_channel);
        }
    } else if (pcmChunk && pcmChunk->fragment && pcmChunk->fragment->payload) {
        const int16_t *samples = (const int16_t *)pcmChunk->fragment->payload; // post-volume
        size_t num_samples_per_channel = pcmChunk->fragment->size / 4;         // stereo int16
        led_controller_feed_audio(samples, num_samples_per_channel);
    }
#endif

    if (xQueueSend(bluetooth_pcm_queue, &pcmChunk, 0) != pdTRUE) {
        // Queue full - drop this chunk to prevent memory buildup
        free_pcm_chunk(pcmChunk);
    }
}


