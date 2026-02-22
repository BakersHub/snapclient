/*
    Wifi related functionality
    Connect to pre defined wifi

    Must be taken over/merge with wifi provision
*/
#include "wifi_interface.h"

#include <string.h>
#include "system_config.h"

// #include "esp_event.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#if ENABLE_WIFI_PROVISIONING
#include "wifi_provisioning.h"
#endif

static const char *TAG = "WIFI";

static char mac_address[18];

EventGroupHandle_t s_wifi_event_group;

static int s_retry_num = 0;

static esp_netif_t *esp_wifi_netif = NULL;
static esp_netif_t *esp_wifi_ap_netif = NULL;
static bool s_ap_started = false;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */

// Event handler for catching system events
static void event_handler(void *arg, esp_event_base_t event_base, int event_id,
                          void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Connected with IP Address:" IPSTR,
             IP2STR(&event->ip_info.ip));

    s_retry_num = 0;
    // Signal main application to continue execution
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    int effective_max_retry = WIFI_MAXIMUM_RETRY;
    if (effective_max_retry <= 0) {
      // Treat 0 or negative as a finite default so we can
      // still fall back to AP mode after some failures.
      effective_max_retry = 5;
    }

    if (s_retry_num < effective_max_retry) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "retry to connect to the AP");
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
      // Automatic fallback: start recovery AP when STA has failed
      ESP_LOGW(TAG,
               "WiFi STA failed after %d retries, starting recovery AP mode",
               s_retry_num);
      wifi_start_ap_mode();
    }
    ESP_LOGI(TAG, "connect to the AP fail");
  }
}

void wifi_init(void) {
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  ESP_ERROR_CHECK(esp_event_handler_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, (esp_event_handler_t)&event_handler, NULL));
  ESP_ERROR_CHECK(
      esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                 (esp_event_handler_t)&event_handler, NULL));

  esp_wifi_netif = esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // esp_wifi_set_bandwidth (WIFI_IF_STA, WIFI_BW_HT20);
  esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
  esp_wifi_set_protocol(
      WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

  // Disable Wi-Fi power save mode for lowest latency
  esp_wifi_set_ps(WIFI_PS_NONE);

  // esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  //   esp_wifi_set_ps(WIFI_PS_NONE);

#if ENABLE_WIFI_PROVISIONING
  /* Start Wi-Fi station */
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  wifi_config_t wifi_config;
  ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifi_config));
  wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "Starting provisioning");

  improv_init();
#else
  // Load only NVS-backed WiFi credentials (no compile-time defaults here)
  system_config_t syscfg;
  memset(&syscfg, 0, sizeof(syscfg));
  system_config_load_from_nvs(&syscfg);

  bool wifi_configured = (syscfg.wifi_ssid[0] != '\0');

  // If WiFi has never been configured via the web UI, immediately
  // start the recovery AP so the user can enter credentials.
  if (!wifi_configured) {
    ESP_LOGW(TAG,
             "WiFi SSID not found in NVS system config, starting recovery AP mode");
    wifi_start_ap_mode();
    return;
  }

  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = WIFI_SSID,
              .password = WIFI_PASSWORD,
              .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
              .threshold.authmode = WIFI_AUTH_WPA2_PSK,
              .pmf_cfg = {.capable = true, .required = false},
          },
  };

  // Override SSID/password from system_config if present in NVS
  if (syscfg.wifi_ssid[0] != '\0') {
    strncpy((char *)wifi_config.sta.ssid, syscfg.wifi_ssid,
            sizeof(wifi_config.sta.ssid));
  }
  if (syscfg.wifi_password[0] != '\0') {
    strncpy((char *)wifi_config.sta.password, syscfg.wifi_password,
            sizeof(wifi_config.sta.password));
  }

  /* Start Wi-Fi station */
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_sta finished.");
#endif

  /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or
   * connection failed for the maximum number of re-tries (WIFI_FAIL_BIT). The
   * bits are set by event_handler() (see above) */
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, portMAX_DELAY);

  /* xEventGroupWaitBits() returns the bits before the call returned, hence we
   * can test which event actually happened. */
  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "connected to ap SSID: %s", wifi_config.sta.ssid);
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
             wifi_config.sta.ssid, wifi_config.sta.password);
  } else {
    ESP_LOGE(TAG, "UNEXPECTED EVENT");
  }

  uint8_t base_mac[6];
  // Get MAC address for WiFi station
  esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
  sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0],
          base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);

  // ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT,
  // IP_EVENT_STA_GOT_IP, &event_handler));
  // ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
  // &event_handler)); vEventGroupDelete(s_wifi_event_group);
}

esp_netif_t *get_current_netif(void) { return esp_wifi_netif; }

void wifi_start_ap_mode(void) {
  if (s_ap_started) {
    ESP_LOGI(TAG, "WiFi AP mode already started");
    return;
  }

  // Create default AP netif if not yet created
  if (esp_wifi_ap_netif == NULL) {
    esp_wifi_ap_netif = esp_netif_create_default_wifi_ap();
    if (esp_wifi_ap_netif == NULL) {
      ESP_LOGE(TAG, "Failed to create default WiFi AP netif");
      return;
    }
  }

  wifi_config_t ap_config = { 0 };

  uint8_t base_mac[6];
  esp_read_mac(base_mac, ESP_MAC_WIFI_SOFTAP);

  snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid),
           "ESP32-SNAPCLIENT-%02X%02X%02X",
           base_mac[3], base_mac[4], base_mac[5]);
  ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);

  ap_config.ap.channel = 1;
  ap_config.ap.max_connection = 4;
    // Open AP (no password)
    ap_config.ap.password[0] = '\0';
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

  // Switch to AP+STA mode so existing station connection (if any) is kept
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set WiFi mode APSTA: %s", esp_err_to_name(err));
    return;
  }

  err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set AP config: %s", esp_err_to_name(err));
    return;
  }

  err = esp_wifi_start();
  if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
    // ESP_ERR_WIFI_CONN is returned if WiFi is already started; ignore it
    ESP_LOGE(TAG, "Failed to start WiFi with AP: %s", esp_err_to_name(err));
    return;
  }

  s_ap_started = true;
  ESP_LOGI(TAG, "Recovery SoftAP started, SSID: %s", ap_config.ap.ssid);
}
