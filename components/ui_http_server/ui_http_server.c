/* HTTP Server Example

         This example code is in the Public Domain (or CC0 licensed, at your
   option.)

         Unless required by applicable law or agreed to in writing, this
         software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
         CONDITIONS OF ANY KIND, either express or implied.
*/

#include "ui_http_server.h"

#include <inttypes.h>
#include <math.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include "dsp_processor.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "system_config.h"

#ifdef CONFIG_ENABLE_LED_CONTROLLER
#include "led_controller.h"
#include "led_config_nvs.h"
#endif

static const char *TAG = "HTTP";

static QueueHandle_t xQueueHttp;

static esp_netif_t *netInterface = NULL;

/*
 * Decode URL-encoded form values in-place (e.g. "JBL%20adapter" -> "JBL adapter").
 * Handles %HH hex escapes and '+' as space.
 */
static void url_decode_inplace(char *str) {
  char *src = str;
  char *dst = str;

  while (*src) {
    if (*src == '+') {
      *dst++ = ' ';
      src++;
    } else if (*src == '%' && isxdigit((unsigned char)src[1]) &&
               isxdigit((unsigned char)src[2])) {
      char hex[3] = {src[1], src[2], '\0'};
      *dst++ = (char)strtol(hex, NULL, 16);
      src += 3;
    } else {
      *dst++ = *src++;
    }
  }
  *dst = '\0';
}

/**
 *
 */
static void SPIFFS_Directory(char *path) {
  DIR *dir = opendir(path);
  assert(dir != NULL);
  while (true) {
    struct dirent *pe = readdir(dir);
    if (!pe) break;
    ESP_LOGI(TAG, "d_name=%s/%s d_ino=%d d_type=%x", path, pe->d_name,
             pe->d_ino, pe->d_type);
  }
  closedir(dir);
}

/**
 *
 */
static esp_err_t SPIFFS_Mount(char *path, char *label, int max_files) {
  esp_vfs_spiffs_conf_t conf = {.base_path = path,
                                .partition_label = label,
                                .max_files = max_files,
                                .format_if_mount_failed = true};

  // Use settings defined above to initialize and mount SPIFFS file system.
  // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
  esp_err_t ret = esp_vfs_spiffs_register(&conf);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount or format filesystem");
    } else if (ret == ESP_ERR_NOT_FOUND) {
      ESP_LOGE(TAG, "Failed to find SPIFFS partition");
    } else {
      ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
    }
    return ret;
  }

  size_t total = 0, used = 0;
  ret = esp_spiffs_info(conf.partition_label, &total, &used);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)",
             esp_err_to_name(ret));
  } else {
    ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
  }

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Mount %s to %s success", path, label);
    SPIFFS_Directory(path);
  }

  return ret;
}

/*
 * DSP configuration GET handler (returns current EQ gains as seen by DSP)
 */
static esp_err_t dsp_config_get_handler(httpd_req_t *req) {
  float fc1 = 300.0f;
  float gain1 = 0.0f;
  float fc2 = 1000.0f;
  float gain2 = 0.0f;
  float fc3 = 4000.0f;
  float gain3 = 0.0f;

  nvs_handle_t nvs;
  esp_err_t err = nvs_open("dspcfg", NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    size_t len = sizeof(float);
    if (nvs_get_blob(nvs, "fc1", &fc1, &len) != ESP_OK) {
      fc1 = 300.0f;
    }

    len = sizeof(float);
    if (nvs_get_blob(nvs, "g1", &gain1, &len) != ESP_OK) {
      gain1 = 0.0f;
    }

    len = sizeof(float);
    if (nvs_get_blob(nvs, "fc2", &fc2, &len) != ESP_OK) {
      fc2 = 1000.0f;
    }

    len = sizeof(float);
    if (nvs_get_blob(nvs, "g2", &gain2, &len) != ESP_OK) {
      gain2 = 0.0f;
    }

    len = sizeof(float);
    if (nvs_get_blob(nvs, "fc3", &fc3, &len) != ESP_OK) {
      fc3 = 4000.0f;
    }

    len = sizeof(float);
    if (nvs_get_blob(nvs, "g3", &gain3, &len) != ESP_OK) {
      gain3 = 0.0f;
    }

    nvs_close(nvs);
  }

  char json[128];
  snprintf(json, sizeof(json),
           "{\"fc1\":%.2f,\"gain1\":%.2f,\"fc2\":%.2f,\"gain2\":%.2f,\"fc3\":%.2f,\"gain3\":%.2f}",
           (double)fc1, (double)gain1, (double)fc2, (double)gain2,
           (double)fc3, (double)gain3);

  ESP_LOGI(TAG,
           "DSP config GET: fc1=%.2f g1=%.2f fc2=%.2f g2=%.2f fc3=%.2f g3=%.2f (err=%s)",
           (double)fc1, (double)gain1, (double)fc2, (double)gain2,
           (double)fc3, (double)gain3,
           esp_err_to_name(err));

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  return ESP_OK;
}

/**
 *
 */
static int find_key_value(char *key, char *parameter, char *value) {
  // char * addr1;
  char *addr1 = strstr(parameter, key);
  if (addr1 == NULL) return 0;
  ESP_LOGD(TAG, "addr1=%s", addr1);

  char *addr2 = addr1 + strlen(key);
  ESP_LOGD(TAG, "addr2=[%s]", addr2);

  char *addr3 = strstr(addr2, "&");
  ESP_LOGD(TAG, "addr3=%p", addr3);
  if (addr3 == NULL) {
    strcpy(value, addr2);
  } else {
    int length = addr3 - addr2;
    ESP_LOGD(TAG, "addr2=%p addr3=%p length=%d", addr2, addr3, length);
    strncpy(value, addr2, length);
    value[length] = 0;
  }
  //	ESP_LOGI(TAG, "key=[%s] value=[%s]", key, value);
  return strlen(value);
}

/**
 *
 */
static esp_err_t Text2Html(httpd_req_t *req, char *filename) {
  //	ESP_LOGI(TAG, "Reading %s", filename);
  FILE *fhtml = fopen(filename, "r");
  if (fhtml == NULL) {
    ESP_LOGE(TAG, "fopen fail. [%s]", filename);
    return ESP_FAIL;
  } else {
    // Send file in chunks without line-by-line processing
    char buffer[1024];
    size_t read_bytes;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), fhtml)) > 0) {
      esp_err_t ret = httpd_resp_send_chunk(req, buffer, read_bytes);
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_resp_send_chunk fail %d", ret);
        fclose(fhtml);
        return ESP_FAIL;
      }
    }
    fclose(fhtml);
  }
  return ESP_OK;
}

/**
 *
 */
#if 0
static esp_err_t Image2Html(httpd_req_t *req, char *filename, char *type) {
  FILE *fhtml = fopen(filename, "r");
  if (fhtml == NULL) {
    ESP_LOGE(TAG, "fopen fail. [%s]", filename);
    return ESP_FAIL;
  } else {
    char buffer[64];

    if (strcmp(type, "jpeg") == 0) {
      httpd_resp_sendstr_chunk(req, "<img src=\"data:image/jpeg;base64,");
    } else if (strcmp(type, "jpg") == 0) {
      httpd_resp_sendstr_chunk(req, "<img src=\"data:image/jpeg;base64,");
    } else if (strcmp(type, "png") == 0) {
      httpd_resp_sendstr_chunk(req, "<img src=\"data:image/png;base64,");
    } else {
      ESP_LOGW(TAG, "file type fail. [%s]", type);
      httpd_resp_sendstr_chunk(req, "<img src=\"data:image/png;base64,");
    }
    while (1) {
      size_t bufferSize = fread(buffer, 1, sizeof(buffer), fhtml);
      ESP_LOGD(TAG, "bufferSize=%d", bufferSize);
      if (bufferSize > 0) {
        httpd_resp_send_chunk(req, buffer, bufferSize);
      } else {
        break;
      }
    }
    fclose(fhtml);
    httpd_resp_sendstr_chunk(req, "\">");
  }
  return ESP_OK;
}
#endif

/**
 * HTTP get handler
 */
static esp_err_t root_get_handler(httpd_req_t *req) {
  //	ESP_LOGI(TAG, "root_get_handler req->uri=[%s]", req->uri);

  /* Send index.html */
  Text2Html(req, "/html/index.html");

  /* Send Image */
  // Image2Html(req, "/html/ESP-LOGO.txt", "png");

  /* Send empty chunk to signal HTTP response completion */
  httpd_resp_sendstr_chunk(req, NULL);

  return ESP_OK;
}

/*
 * HTTP post handler
 */
static esp_err_t root_post_handler(httpd_req_t *req) {
  //	ESP_LOGI(TAG, "root_post_handler req->uri=[%s]", req->uri);
  URL_t urlBuf;
  int ret = -1;

  memset(&urlBuf, 0, sizeof(URL_t));

  if (find_key_value("gain_1=", (char *)req->uri, urlBuf.str_value)) {
    ESP_LOGD(TAG, "urlBuf.str_value=[%s]", urlBuf.str_value);

    urlBuf.gain_1 = strtof(urlBuf.str_value, NULL);
    ESP_LOGD(TAG, "urlBuf.float_value=%f", urlBuf.gain_1);

    ret = 0;
  } else {
    ESP_LOGD(TAG, "key 'gain_1=' not found");
  }

  if (find_key_value("gain_2=", (char *)req->uri, urlBuf.str_value)) {
    ESP_LOGD(TAG, "urlBuf.str_value=[%s]", urlBuf.str_value);

    urlBuf.gain_2 = strtof(urlBuf.str_value, NULL);
    ESP_LOGD(TAG, "urlBuf.float_value=%f", urlBuf.gain_2);

    ret = 0;
  } else {
    ESP_LOGD(TAG, "key 'gain_2=' not found");
  }

  if (find_key_value("gain_3=", (char *)req->uri, urlBuf.str_value)) {
    ESP_LOGD(TAG, "urlBuf.str_value=[%s]", urlBuf.str_value);

    urlBuf.gain_3 = strtof(urlBuf.str_value, NULL);
    ESP_LOGD(TAG, "urlBuf.float_value=%f", urlBuf.gain_3);

    ret = 0;
  } else {
    ESP_LOGD(TAG, "key 'gain_3=' not found");
  }

  if (ret >= 0) {
    // Send to http_server_task
    if (xQueueSend(xQueueHttp, &urlBuf, portMAX_DELAY) != pdPASS) {
      ESP_LOGE(TAG, "xQueueSend Fail");
    }

    // Also persist latest gains to NVS so they can be
    // queried by /dsp/config even if the DSP worker
    // hasn't yet written them.
    nvs_handle_t nvs;
    esp_err_t nvs_err = nvs_open("dspcfg", NVS_READWRITE, &nvs);
    if (nvs_err == ESP_OK) {
      float g1 = urlBuf.gain_1;
      float g2 = urlBuf.gain_2;
      float g3 = urlBuf.gain_3;

      // Store only gains here; frequencies keep defaults
      // used by the DSP processor (300 Hz / 1 kHz / 4 kHz).
      if (nvs_set_blob(nvs, "g1", &g1, sizeof(float)) == ESP_OK &&
          nvs_set_blob(nvs, "g2", &g2, sizeof(float)) == ESP_OK &&
          nvs_set_blob(nvs, "g3", &g3, sizeof(float)) == ESP_OK) {
        nvs_commit(nvs);
      }

      nvs_close(nvs);
    } else {
      ESP_LOGW(TAG, "Failed to open DSP NVS for write from HTTP: %s",
               esp_err_to_name(nvs_err));
    }
  }

  /* Redirect onto root to see the updated file list */
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
  httpd_resp_set_hdr(req, "Connection", "close");
#endif
  httpd_resp_sendstr(req, "post successfully");
  return ESP_OK;
}

/*
 * favicon get handler
 */
static esp_err_t favicon_get_handler(httpd_req_t *req) {
  //	ESP_LOGI(TAG, "favicon_get_handler req->uri=[%s]", req->uri);
  return ESP_OK;
}

#ifdef CONFIG_ENABLE_LED_CONTROLLER
/*
 * LED control POST handler
 */
static esp_err_t led_post_handler(httpd_req_t *req) {
  char buf[200];
  int ret;

  int remaining = req->content_len;
  if (remaining >= sizeof(buf)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
    return ESP_FAIL;
  }

  ret = httpd_req_recv(req, buf, remaining);
  if (ret <= 0) {
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
      httpd_resp_send_408(req);
    }
    return ESP_FAIL;
  }
  buf[ret] = '\0';

  ESP_LOGI(TAG, "LED POST: %s", buf);

  // Parse parameters
  char value[32];
  
  if (find_key_value("effect=", buf, value)) {
    int effect = atoi(value);
    led_controller_set_effect((led_effect_t)effect);
    ESP_LOGI(TAG, "LED effect set to %d", effect);
  }
  
  if (find_key_value("brightness=", buf, value)) {
    int brightness = atoi(value);
    led_controller_set_brightness((uint8_t)brightness);
    ESP_LOGI(TAG, "LED brightness set to %d", brightness);
  }
  
  if (find_key_value("speed=", buf, value)) {
    int speed = atoi(value);
    led_controller_set_speed((uint8_t)speed);
    ESP_LOGI(TAG, "LED speed set to %d", speed);
  }
  
  if (find_key_value("sensitivity=", buf, value)) {
    int sensitivity = atoi(value);
    led_controller_set_sensitivity((uint8_t)sensitivity);
    ESP_LOGI(TAG, "LED sensitivity set to %d", sensitivity);
  }

  if (find_key_value("bass_focus=", buf, value)) {
    int bass_focus = atoi(value);
    if (bass_focus < 0) bass_focus = 0;
    if (bass_focus > 255) bass_focus = 255;
    led_controller_set_bass_focus((uint8_t)bass_focus);
    ESP_LOGI(TAG, "LED bass_focus set to %d", bass_focus);
  }

  if (find_key_value("auto_rainbow=", buf, value)) {
    uint8_t enabled = (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) ? 1 : 0;
    led_controller_set_auto_rainbow(enabled);
    ESP_LOGI(TAG, "LED auto_rainbow set to %d", enabled);
  }

  if (find_key_value("rainbow_speed=", buf, value)) {
    int rs = atoi(value);
    if (rs < 0) rs = 0;
    if (rs > 255) rs = 255;
    led_controller_set_rainbow_speed((uint8_t)rs);
    ESP_LOGI(TAG, "LED rainbow_speed set to %d", rs);
  }
  
  if (find_key_value("auto_rainbow2=", buf, value)) {
    uint8_t enabled2 = (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) ? 1 : 0;
    led_controller_set_auto_rainbow2(enabled2);
    ESP_LOGI(TAG, "LED auto_rainbow2 set to %d", enabled2);
  }

  if (find_key_value("rainbow_speed2=", buf, value)) {
    int rs2 = atoi(value);
    if (rs2 < 0) rs2 = 0;
    if (rs2 > 255) rs2 = 255;
    led_controller_set_rainbow_speed2((uint8_t)rs2);
    ESP_LOGI(TAG, "LED rainbow_speed2 set to %d", rs2);
  }
  
  // Parse color (format: r,g,b)
  if (find_key_value("color=", buf, value)) {
    int r, g, b;
    if (sscanf(value, "%d,%d,%d", &r, &g, &b) == 3) {
      led_color_t color1 = {.r = r, .g = g, .b = b};
      led_color_t color2 = {0};

      // Optional secondary color (format: r,g,b)
      if (find_key_value("color2=", buf, value)) {
        int r2, g2, b2;
        if (sscanf(value, "%d,%d,%d", &r2, &g2, &b2) == 3) {
          color2.r = r2;
          color2.g = g2;
          color2.b = b2;
        }
      }

      led_controller_set_color(color1, color2);
      ESP_LOGI(TAG, "LED color set to %d,%d,%d", r, g, b);
    }
  }

  httpd_resp_set_status(req, "200 OK");
  httpd_resp_sendstr(req, "LED settings updated");
  return ESP_OK;
}

/*
 * LED status GET handler
 */
static esp_err_t led_get_handler(httpd_req_t *req) {
  led_config_t config;
  esp_err_t res = led_controller_get_config(&config);
  if (res != ESP_OK) {
    if (res == ESP_ERR_INVALID_STATE) {
      // LED controller not initialized: return a safe default JSON
      memset(&config, 0, sizeof(config));
    } else {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get LED config");
      return ESP_FAIL;
    }
  }

  char json[640];
  snprintf(json, sizeof(json),
           "{\"effect\":%d,\"brightness\":%d,\"speed\":%d,\"sensitivity\":%d,\"bass_focus\":%d,"
           "\"color\":{\"r\":%d,\"g\":%d,\"b\":%d},"
           "\"color2\":{\"r\":%d,\"g\":%d,\"b\":%d},\"num_leds\":%d,\"enabled\":%s,\"color_order\":%d,\"ignore_volume\":%s,\"auto_rainbow\":%s,\"rainbow_speed\":%d,\"auto_rainbow2\":%s,\"rainbow_speed2\":%d}",
           config.effect, config.brightness, config.speed, config.sensitivity, config.bass_focus,
           config.color1.r, config.color1.g, config.color1.b,
           config.color2.r, config.color2.g, config.color2.b,
           config.num_leds,
           (config.num_leds == 0) ? "false" : "true",
           config.color_order,
           config.ignore_volume ? "true" : "false",
           config.auto_rainbow ? "true" : "false",
           config.rainbow_speed,
           config.auto_rainbow2 ? "true" : "false",
           config.rainbow_speed2);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  return ESP_OK;
}

/*
 * LED config POST handler - saves to NVS and restarts
 */
static void restart_task(void *arg) {
  vTaskDelay(pdMS_TO_TICKS(2000));  // 2 second delay
  esp_restart();
}

static esp_err_t led_config_handler(httpd_req_t *req) {
  char content[512];
  size_t total_len = req->content_len;
  if (total_len >= sizeof(content)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
    return ESP_FAIL;
  }

  size_t received = 0;
  while (received < total_len) {
    int ret = httpd_req_recv(req, content + received, total_len - received);
    if (ret <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        httpd_resp_send_408(req);
      }
      return ESP_FAIL;
    }
    received += ret;
  }
  content[received] = '\0';
  
  led_config_nvs_t config;
  led_config_get_defaults(&config);  // Start with defaults
  
  char value[32];
  if (find_key_value("num_leds=", content, value)) {
    config.num_leds = atoi(value);
  }
  if (find_key_value("gpio_pin=", content, value)) {
    config.gpio_pin = atoi(value);
  }
  if (find_key_value("enabled=", content, value)) {
    config.enabled = (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) ? 1 : 0;
  }
  if (find_key_value("color_order=", content, value)) {
    int co = atoi(value);
    config.color_order = (co == 1) ? 1 : 0; // clamp to RGB(0) or GRB(1)
  }
  if (find_key_value("ignore_volume=", content, value)) {
    config.ignore_volume = (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) ? 1 : 0;
  }
  if (find_key_value("bass_focus=", content, value)) {
    int bf = atoi(value);
    if (bf < 0) bf = 0;
    if (bf > 255) bf = 255;
    config.bass_focus = (uint8_t)bf;
  }
  if (find_key_value("auto_rainbow=", content, value)) {
    config.auto_rainbow = (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) ? 1 : 0;
  }
  if (find_key_value("rainbow_speed=", content, value)) {
    int rs = atoi(value);
    if (rs < 0) rs = 0;
    if (rs > 255) rs = 255;
    config.rainbow_speed = (uint8_t)rs;
  }
  if (find_key_value("auto_rainbow2=", content, value)) {
    config.auto_rainbow2 = (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) ? 1 : 0;
  }
  if (find_key_value("rainbow_speed2=", content, value)) {
    int rs2 = atoi(value);
    if (rs2 < 0) rs2 = 0;
    if (rs2 > 255) rs2 = 255;
    config.rainbow_speed2 = (uint8_t)rs2;
  }
  if (find_key_value("brightness=", content, value)) {
    config.brightness = atoi(value);
  }
  if (find_key_value("effect=", content, value)) {
    config.effect = atoi(value);
  }
  if (find_key_value("speed=", content, value)) {
    config.speed = atoi(value);
  }
  if (find_key_value("sensitivity=", content, value)) {
    config.sensitivity = atoi(value);
  }

  // Optional primary color (format: r,g,b)
  if (find_key_value("color=", content, value)) {
    int r, g, b;
    if (sscanf(value, "%d,%d,%d", &r, &g, &b) == 3) {
      config.color_r = r;
      config.color_g = g;
      config.color_b = b;
    }
  }

  // Optional secondary color (format: r,g,b)
  if (find_key_value("color2=", content, value)) {
    int r2, g2, b2;
    if (sscanf(value, "%d,%d,%d", &r2, &g2, &b2) == 3) {
      config.color2_r = r2;
      config.color2_g = g2;
      config.color2_b = b2;
    }
  }
  
  // Save to NVS
  if (led_config_save_to_nvs(&config) == ESP_OK) {
    ESP_LOGI(TAG, "LED config saved, will restart in 2 seconds...");
    
    // Send response first
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Config saved! Restarting in 2 seconds...");
    
    // Create task to restart after delay (allows response to be sent)
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    
    return ESP_OK;
  } else {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save config");
    return ESP_FAIL;
  }
}
#endif // CONFIG_ENABLE_LED_CONTROLLER

/*
 * System configuration GET handler
 */
static esp_err_t system_config_get_handler(httpd_req_t *req) {
  system_config_t cfg;
  system_config_set_defaults(&cfg);
  system_config_load_from_nvs(&cfg);

  char json[1280];
  snprintf(json, sizeof(json),
           "{\"snapclient_name\":\"%s\",\"snapcast_gain_boost\":%.3f,"
           "\"wifi_ssid\":\"%s\",\"wifi_password\":\"%s\"," 
           "\"snapserver_host\":\"%s\",\"snapserver_port\":%d,"
           "\"volume_buttons_enabled\":%s,\"volume_up_pin\":%d,\"volume_down_pin\":%d,"
           "\"effect_button_enabled\":%s,\"effect_button_pin\":%d,"
           "\"sh1106_enabled\":%s,\"sh1106_sda_gpio\":%d,\"sh1106_scl_gpio\":%d,\"sh1106_i2c_freq_hz\":%d,"
           "\"sh1106_column_offset\":%d,"
           "\"i2s_mclk_pin\":%d,\"i2s_bck_pin\":%d,\"i2s_lrck_pin\":%d,\"i2s_dataout_pin\":%d,"
           "\"pcm5102a_mute_pin\":%d}",
           cfg.snapclient_name,
           (double)cfg.snapcast_gain_boost,
           cfg.wifi_ssid,
           cfg.wifi_password,
           cfg.snapserver_host,
           cfg.snapserver_port,
           cfg.volume_buttons_enabled ? "true" : "false",
           cfg.volume_up_pin,
           cfg.volume_down_pin,
           cfg.effect_button_enabled ? "true" : "false",
           cfg.effect_button_pin,
           cfg.sh1106_enabled ? "true" : "false",
           cfg.sh1106_sda_gpio,
           cfg.sh1106_scl_gpio,
           cfg.sh1106_i2c_freq_hz,
           cfg.sh1106_column_offset,
           cfg.i2s_mclk_pin,
           cfg.i2s_bck_pin,
           cfg.i2s_lrck_pin,
           cfg.i2s_dataout_pin,
           cfg.pcm5102a_mute_pin);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  return ESP_OK;
}

/*
 * System configuration POST handler - saves to NVS and restarts
 */
static esp_err_t system_config_post_handler(httpd_req_t *req) {
  // Allow a reasonably large config payload so all fields
  // (including newer ones like sh1106_column_offset) are received.
  char content[512];
  size_t total_len = req->content_len;
  if (total_len >= sizeof(content)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
    return ESP_FAIL;
  }

  size_t received = 0;
  while (received < total_len) {
    int ret = httpd_req_recv(req, content + received, total_len - received);
    if (ret <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        httpd_resp_send_408(req);
      }
      return ESP_FAIL;
    }
    received += ret;
  }
  content[received] = '\0';

  system_config_t cfg;
  system_config_set_defaults(&cfg);
  system_config_load_from_nvs(&cfg);

  char value[64];

  if (find_key_value("snapclient_name=", content, value)) {
    url_decode_inplace(value);
    strncpy(cfg.snapclient_name, value, SYSTEM_CONFIG_MAX_NAME_LEN - 1);
    cfg.snapclient_name[SYSTEM_CONFIG_MAX_NAME_LEN - 1] = '\0';
  }

  if (find_key_value("snapcast_gain_boost=", content, value)) {
    float g = (float)atof(value);
    if (g > 0.0f && g < 10.0f) {
      cfg.snapcast_gain_boost = g;
    }
  }

  if (find_key_value("wifi_ssid=", content, value)) {
    url_decode_inplace(value);
    strncpy(cfg.wifi_ssid, value, SYSTEM_CONFIG_MAX_SSID_LEN - 1);
    cfg.wifi_ssid[SYSTEM_CONFIG_MAX_SSID_LEN - 1] = '\0';
  }

  if (find_key_value("wifi_password=", content, value)) {
    url_decode_inplace(value);
    strncpy(cfg.wifi_password, value, SYSTEM_CONFIG_MAX_PASS_LEN - 1);
    cfg.wifi_password[SYSTEM_CONFIG_MAX_PASS_LEN - 1] = '\0';
  }

  if (find_key_value("snapserver_host=", content, value)) {
    url_decode_inplace(value);
    strncpy(cfg.snapserver_host, value, SYSTEM_CONFIG_MAX_HOST_LEN - 1);
    cfg.snapserver_host[SYSTEM_CONFIG_MAX_HOST_LEN - 1] = '\0';
  }

  if (find_key_value("snapserver_port=", content, value)) {
    int port = atoi(value);
    if (port > 0 && port <= 65535) {
      cfg.snapserver_port = port;
    }
  }

  if (find_key_value("volume_buttons_enabled=", content, value)) {
    cfg.volume_buttons_enabled = (strcmp(value, "1") == 0);
  }

  if (find_key_value("volume_up_pin=", content, value)) {
    cfg.volume_up_pin = atoi(value);
  }

  if (find_key_value("volume_down_pin=", content, value)) {
    cfg.volume_down_pin = atoi(value);
  }

  if (find_key_value("effect_button_enabled=", content, value)) {
    cfg.effect_button_enabled = (strcmp(value, "1") == 0 || strcmp(value, "true") == 0);
  }

  if (find_key_value("effect_button_pin=", content, value)) {
    cfg.effect_button_pin = atoi(value);
  }

  if (find_key_value("sh1106_enabled=", content, value)) {
    cfg.sh1106_enabled = (strcmp(value, "1") == 0 || strcmp(value, "true") == 0);
  }

  if (find_key_value("sh1106_sda_gpio=", content, value)) {
    cfg.sh1106_sda_gpio = atoi(value);
  }

  if (find_key_value("sh1106_scl_gpio=", content, value)) {
    cfg.sh1106_scl_gpio = atoi(value);
  }

  if (find_key_value("sh1106_i2c_freq_hz=", content, value)) {
    int freq = atoi(value);
    if (freq > 0 && freq <= 1000000) {
      cfg.sh1106_i2c_freq_hz = freq;
    }
  }

  if (find_key_value("sh1106_column_offset=", content, value)) {
    int col = atoi(value);
    if (col >= 0 && col <= 127) {
      cfg.sh1106_column_offset = col;
    }
  }

  if (find_key_value("i2s_mclk_pin=", content, value)) {
    int pin = atoi(value);
    if (pin >= -1 && pin <= 39) {
      cfg.i2s_mclk_pin = pin;
    }
  }

  if (find_key_value("i2s_bck_pin=", content, value)) {
    int pin = atoi(value);
    if (pin >= -1 && pin <= 39) {
      cfg.i2s_bck_pin = pin;
    }
  }

  if (find_key_value("i2s_lrck_pin=", content, value)) {
    int pin = atoi(value);
    if (pin >= -1 && pin <= 39) {
      cfg.i2s_lrck_pin = pin;
    }
  }

  if (find_key_value("i2s_dataout_pin=", content, value)) {
    int pin = atoi(value);
    if (pin >= -1 && pin <= 39) {
      cfg.i2s_dataout_pin = pin;
    }
  }

  if (find_key_value("pcm5102a_mute_pin=", content, value)) {
    int pin = atoi(value);
    if (pin >= -1 && pin <= 39) {
      cfg.pcm5102a_mute_pin = pin;
    }
  }

  if (system_config_save_to_nvs(&cfg) == ESP_OK) {
    ESP_LOGI(TAG,
             "System config saved, will restart in 2 seconds (name='%s', up=%d, down=%d, gain=%.2f, buttons=%s)",
             cfg.snapclient_name,
             cfg.volume_up_pin,
             cfg.volume_down_pin,
             (double)cfg.snapcast_gain_boost,
             cfg.volume_buttons_enabled ? "on" : "off");

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "System config saved! Restarting in 2 seconds...");

    xTaskCreate(restart_task, "restart_sys", 2048, NULL, 5, NULL);
    return ESP_OK;
  }

  httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                      "Failed to save system config");
  return ESP_FAIL;
}

/*
 * Function to start the web server
 */
esp_err_t start_server(const char *base_path, int port) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  // We have several endpoints (/ , /post, /favicon, /led*, /system/config,
  // /dsp/config), so increase the URI handler limit above the default (8).
  config.max_uri_handlers = 10;
  // Allow a couple of concurrent HTTP connections (HTML, /led, /system/config)
  // and purge least-recently-used sockets if the limit is hit. Keep-alive
  // is disabled so sockets are closed promptly after each request, which
  // helps avoid running out of LWIP sockets when multiple clients (PC + phone)
  // connect.
  config.max_open_sockets = 2;
  config.lru_purge_enable = true;
  config.keep_alive_enable = false;

  /* Use the URI wildcard matching function in order to
   * allow the same handler to respond to multiple different
   * target URIs which match the wildcard scheme */
  config.uri_match_fn = httpd_uri_match_wildcard;

  ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start file server!");
    return ESP_FAIL;
  }

  /* URI handler for get */
  httpd_uri_t _root_get_handler = {
      .uri = "/", .method = HTTP_GET, .handler = root_get_handler,
      //.user_ctx  = server_data	// Pass server data as context
  };
  httpd_register_uri_handler(server, &_root_get_handler);

  /* URI handler for post */
  httpd_uri_t _root_post_handler = {
      .uri = "/post", .method = HTTP_POST, .handler = root_post_handler,
      //.user_ctx  = server_data	// Pass server data as context
  };
  httpd_register_uri_handler(server, &_root_post_handler);

  /* URI handler for favicon.ico */
  httpd_uri_t _favicon_get_handler = {
      .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler,
      //.user_ctx  = server_data	// Pass server data as context
  };
  httpd_register_uri_handler(server, &_favicon_get_handler);

#ifdef CONFIG_ENABLE_LED_CONTROLLER
  /* URI handler for LED POST */
  httpd_uri_t _led_post_handler = {
      .uri = "/led", .method = HTTP_POST, .handler = led_post_handler,
  };
  httpd_register_uri_handler(server, &_led_post_handler);

  /* URI handler for LED GET */
  httpd_uri_t _led_get_handler = {
      .uri = "/led", .method = HTTP_GET, .handler = led_get_handler,
  };
  httpd_register_uri_handler(server, &_led_get_handler);

  /* URI handler for LED config POST */
  httpd_uri_t _led_config_handler = {
      .uri = "/led/config", .method = HTTP_POST, .handler = led_config_handler,
  };
  httpd_register_uri_handler(server, &_led_config_handler);
#endif

  /* URI handler for system configuration GET */
  httpd_uri_t _syscfg_get_handler = {
      .uri = "/system/config", .method = HTTP_GET,
      .handler = system_config_get_handler,
  };
  httpd_register_uri_handler(server, &_syscfg_get_handler);

  /* URI handler for system configuration POST */
  httpd_uri_t _syscfg_post_handler = {
      .uri = "/system/config", .method = HTTP_POST,
      .handler = system_config_post_handler,
  };
  httpd_register_uri_handler(server, &_syscfg_post_handler);

  /* URI handler for DSP configuration GET */
  httpd_uri_t _dspcfg_get_handler = {
      .uri = "/dsp/config", .method = HTTP_GET,
      .handler = dsp_config_get_handler,
  };
  esp_err_t dsp_reg_res = httpd_register_uri_handler(server, &_dspcfg_get_handler);
  if (dsp_reg_res != ESP_OK) {
    ESP_LOGW(TAG, "Failed to register /dsp/config handler: %s",
             esp_err_to_name(dsp_reg_res));
  }

  return ESP_OK;
}

//// LEDC Stuff
//#define LEDC_TIMER			LEDC_TIMER_0
//#define LEDC_MODE			LEDC_LOW_SPEED_MODE
////#define LEDC_OUTPUT_IO	(5) // Define the output GPIO
//#define LEDC_OUTPUT_IO		CONFIG_BLINK_GPIO // Define the output
// GPIO #define LEDC_CHANNEL		LEDC_CHANNEL_0 #define LEDC_DUTY_RES
// LEDC_TIMER_13_BIT // Set duty resolution to 13 bits #define LEDC_DUTY
//(4095) // Set duty to 50%. ((2 ** 13) - 1) * 50% = 4095 #define LEDC_FREQUENCY
//(5000) // Frequency in Hertz. Set frequency at 5 kHz
//
// static void ledc_init(void)
//{
//	// Prepare and then apply the LEDC PWM timer configuration
//	ledc_timer_config_t ledc_timer = {
//		.speed_mode			= LEDC_MODE,
//		.timer_num			= LEDC_TIMER,
//		.duty_resolution	= LEDC_DUTY_RES,
//		.freq_hz			= LEDC_FREQUENCY,  // Set output
// frequency at 5 kHz 		.clk_cfg			= LEDC_AUTO_CLK
//	};
//	ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
//
//	// Prepare and then apply the LEDC PWM channel configuration
//	ledc_channel_config_t ledc_channel = {
//		.speed_mode			= LEDC_MODE,
//		.channel			= LEDC_CHANNEL,
//		.timer_sel			= LEDC_TIMER,
//		.intr_type			= LEDC_INTR_DISABLE,
//		.gpio_num			= LEDC_OUTPUT_IO,
//		.duty				= 0, // Set duty to 0%
//		.hpoint				= 0
//	};
//	ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
//}

/**
 *
 */
static void http_server_task(void *pvParameters) {
  /* Get the local IP address */
  esp_netif_ip_info_t ip_info;

  ESP_ERROR_CHECK(esp_netif_get_ip_info(netInterface, &ip_info));

  char ipString[64];
  sprintf(ipString, IPSTR, IP2STR(&ip_info.ip));

  ESP_LOGI(TAG, "Start http task=%s", ipString);

  char portString[6];
  sprintf(portString, "%d", CONFIG_WEB_PORT);

  char url[strlen("http://") + strlen(ipString) + strlen(":") +
           strlen(portString) + 1];
  memset(url, 0, sizeof(url));
  strcat(url, ipString);
  strcat(url, ":");
  strcat(url, portString);

  // Set the LEDC peripheral configuration
  //	ledc_init();

  // Set duty to 50%
  //	double maxduty = pow(2, 13) - 1;
  //	float percent = 0.5;
  //	uint32_t duty = maxduty * percent;
  //	ESP_LOGI(TAG, "duty=%"PRIu32, duty);
  //	ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, LEDC_DUTY));
  //	ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty));
  // Update duty to apply the new value
  //	ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));

  // Start Server
  ESP_LOGI(TAG, "Starting server on %s", url);
  ESP_ERROR_CHECK(start_server("/html", CONFIG_WEB_PORT));

  URL_t urlBuf;
  while (1) {
    //	  ESP_LOGW (TAG, "stack free: %d", uxTaskGetStackHighWaterMark(NULL));

    // Waiting for post
    if (xQueueReceive(xQueueHttp, &urlBuf, portMAX_DELAY) == pdTRUE) {
      filterParams_t filterParams;

      ESP_LOGI(TAG, "str_value=%s gain_1=%f, gain_2=%f, gain_3=%f",
               urlBuf.str_value, urlBuf.gain_1, urlBuf.gain_2, urlBuf.gain_3);

      filterParams.dspFlow = dspfEQBassTreble;
      filterParams.fc_1 = 300.0;
      filterParams.gain_1 = urlBuf.gain_1;
      filterParams.fc_2 = 1000.0;
      filterParams.gain_2 = urlBuf.gain_2;
      filterParams.fc_3 = 4000.0;
      filterParams.gain_3 = urlBuf.gain_3;

#if CONFIG_USE_DSP_PROCESSOR
      dsp_processor_update_filter_params(&filterParams);
#endif

      // Set duty value
      //			percent = urlBuf.long_value / 100.0;
      //			duty = maxduty * percent;
      //			ESP_LOGI(TAG, "percent=%f duty=%"PRIu32,
      // percent, duty);
      // ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty));
      // Update duty to apply the new value
      //			ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE,
      // LEDC_CHANNEL));
    }
  }

  // Never reach here
  ESP_LOGI(TAG, "finish");
  vTaskDelete(NULL);
}

/**
 *
 */
void init_http_server_task(char *key) {
  if (!key) {
    ESP_LOGE(TAG,
             "key should be \"WIFI_STA_DEF\", \"WIFI_AP_DEF\" or \"ETH_DEF\"");
    return;
  }

  netInterface = esp_netif_get_handle_from_ifkey(key);
  if (!netInterface) {
    ESP_LOGE(TAG, "can't get net interface for %s", key);
    return;
  }

  // Initialize SPIFFS
  ESP_LOGI(TAG, "Initializing SPIFFS");
  if (SPIFFS_Mount("/html", "storage", 6) != ESP_OK) {
    ESP_LOGE(TAG, "SPIFFS mount failed");
    return;
  }

  // Create Queue
  xQueueHttp = xQueueCreate(10, sizeof(URL_t));
  configASSERT(xQueueHttp);

  xTaskCreatePinnedToCore(http_server_task, "HTTP", 512 * 5, NULL, 2, NULL,
                          tskNO_AFFINITY);
}
