/* Play flac file by audio pipeline
   This example code is in the Public Domain (or CC0 licensed, at your option.)
   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdint.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "hal/gpio_types.h"
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
#include "eth_interface.h"
#endif

#include "nvs_flash.h"
#include "wifi_interface.h"

// Minimum ESP-IDF stuff only hardware abstraction stuff
#include <wifi_provisioning.h>

#if CONFIG_BT_ENABLED
#include "bt_audio_main.h"
#include "bt_audio_sink.h"
#endif

#include "board.h"
#include "es8388.h"
#include "esp_netif.h"
#include "lwip/api.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "mdns.h"
#include "net_functions.h"

// Web socket server
// #include "websocket_if.h"
// #include "websocket_server.h"

#include <sys/time.h>

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_http_client.h"
#include "cJSON.h"
#if CONFIG_USE_DSP_PROCESSOR
#include "dsp_processor.h"
#endif

#ifdef CONFIG_ENABLE_LED_CONTROLLER
#include "led_controller.h"
#include "led_config_nvs.h"
#endif

// Opus decoder is implemented as a subcomponet from master git repo
#include "opus.h"

// flac decoder is implemented as a subcomponet from master git repo
#include "FLAC/stream_decoder.h"
#include "ota_server.h"
#include "player.h"
#include "snapcast.h"
#include "ui_http_server.h"

#if CONFIG_ENABLE_SH1106_DISPLAY
#include "display_sh1106.h"
#endif

#include "system_config.h"

static system_config_t g_system_config;

// Volume control GPIO definitions are taken from system configuration
#define VOLUME_UP_GPIO   (g_system_config.volume_up_pin)
#define VOLUME_DOWN_GPIO (g_system_config.volume_down_pin)

// LED effect toggle button GPIO from system configuration
#define LED_EFFECT_BUTTON_GPIO (g_system_config.effect_button_pin)

#define VOLUME_BUTTON_DEBOUNCE_MS 100  // Volume button debounce - reduced for instant feel
#define VOLUME_REPEAT_RATE_MS 500      // Repeat rate when holding button

// Recovery AP trigger button (configurable, active-high)
// Wire a momentary button so that pressing it drives the GPIO HIGH.
#define AP_MODE_BUTTON_GPIO ((gpio_num_t)g_system_config.ap_mode_button_gpio)
#define AP_MODE_HOLD_MS     3000

static bool isCachedChunk = false;
static uint32_t cachedBlocks = 0;

static FLAC__StreamDecoderReadStatus read_callback(
    const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes,
    void *client_data);
static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data);
static void metadata_callback(const FLAC__StreamDecoder *decoder,
                              const FLAC__StreamMetadata *metadata,
                              void *client_data);
static void error_callback(const FLAC__StreamDecoder *decoder,
                           FLAC__StreamDecoderErrorStatus status,
                           void *client_data);

/* Volume control function declarations */
static void init_volume_buttons(void);
static void volume_button_task(void *pvParameters);
static void send_volume_update_to_server(int volume_percent);

#ifdef CONFIG_ENABLE_LED_CONTROLLER
/* LED effect toggle button declarations */
static void init_effect_button(void);
static void effect_button_task(void *pvParameters);
#endif

/* AP mode trigger from GPIO0 */
static void ap_mode_button_task(void *pvParameters);

#if CONFIG_ENABLE_SH1106_DISPLAY
/* WiFi signal monitoring function declarations */
static void update_wifi_signal_display(void);
static void wifi_signal_monitor_task(void *pvParameters);
#endif

static FLAC__StreamDecoder *flacDecoder = NULL;

const char *VERSION_STRING = "0.0.3";

#define HTTP_TASK_PRIORITY 9
#define HTTP_TASK_CORE_ID tskNO_AFFINITY

#define OTA_TASK_PRIORITY 6
#define OTA_TASK_CORE_ID tskNO_AFFINITY
// 1  // tskNO_AFFINITY

TaskHandle_t t_ota_task = NULL;
TaskHandle_t t_http_get_task = NULL;

#if CONFIG_ENABLE_SH1106_DISPLAY
/* WiFi signal monitoring */
static TaskHandle_t wifi_monitor_task_handle = NULL;
static int32_t current_rssi = -100;
#endif

#ifdef CONFIG_ENABLE_LED_CONTROLLER
static bool led_initialized = false;
#endif

/* Global snapcast settings for server synchronization */
snapcastSetting_t scSet;

/* Global network connection variables for volume control */
struct netconn *lwipNetconn;
ip_addr_t remote_ip;  // Global snapcast server IP for volume control

#define FAST_SYNC_LATENCY_BUF 20000      // in µs - Less aggressive sync for server buffer compatibility
#define NORMAL_SYNC_LATENCY_BUF 2000000  // in µs - Allow more tolerance for 950ms server buffer

struct timeval tdif, tavg;

/* snapast parameters; configurable in menuconfig */
#define SNAPCAST_SERVER_USE_MDNS CONFIG_SNAPSERVER_USE_MDNS
#if !SNAPCAST_SERVER_USE_MDNS
#define SNAPCAST_SERVER_HOST CONFIG_SNAPSERVER_HOST
#define SNAPCAST_SERVER_PORT CONFIG_SNAPSERVER_PORT
#endif
#define SNAPCAST_USE_SOFT_VOL CONFIG_SNAPCLIENT_USE_SOFT_VOL

/* Logging tag */
static const char *TAG = "SC";

// static QueueHandle_t playerChunkQueueHandle = NULL;
SemaphoreHandle_t timeSyncSemaphoreHandle = NULL;

#if CONFIG_USE_DSP_PROCESSOR
#if CONFIG_SNAPCLIENT_DSP_FLOW_STEREO
dspFlows_t dspFlow = dspfStereo;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BASSBOOST
dspFlows_t dspFlow = dspfBassBoost;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BIAMP
dspFlows_t dspFlow = dspfBiamp;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BASS_TREBLE_EQ
dspFlows_t dspFlow = dspfEQBassTreble;
#endif
#endif

typedef struct audioDACdata_s {
  bool mute;
  int volume;
} audioDACdata_t;

audioDACdata_t audioDAC_data;
static QueueHandle_t audioDACQHdl = NULL;
SemaphoreHandle_t audioDACSemaphore = NULL;

typedef struct decoderData_s {
  uint32_t type;  // should be SNAPCAST_MESSAGE_CODEC_HEADER
                  // or SNAPCAST_MESSAGE_WIRE_CHUNK
  uint8_t *inData;
  tv_t timestamp;
  uint8_t *outData;
  uint32_t bytes;
} decoderData_t;

void time_sync_msg_cb(void *args);

static char base_message_serialized[BASE_MESSAGE_SIZE];

static const esp_timer_create_args_t tSyncArgs = {
    .callback = &time_sync_msg_cb,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "tSyncMsg",
    .skip_unhandled_events = false};

struct netconn *lwipNetconn;

static int id_counter = 0;

static OpusDecoder *opusDecoder = NULL;

static decoderData_t decoderChunk = {
    .type = SNAPCAST_MESSAGE_INVALID,
    .inData = NULL,
    .timestamp = {0, 0},
    .outData = NULL,
    .bytes = 0,
};

static decoderData_t pcmChunk = {
    .type = SNAPCAST_MESSAGE_INVALID,
    .inData = NULL,
    .timestamp = {0, 0},
    .outData = NULL,
    .bytes = 0,
};

/**
 *
 */
void time_sync_msg_cb(void *args) {
  base_message_t base_message_tx;
  //  struct timeval now;
  int64_t now;
  // time_message_t time_message_tx = {{0, 0}};
  int rc1;

  // causes kernel panic, which shouldn't happen though?
  // Isn't it called from timer task instead of ISR?
  // xSemaphoreGive(timeSyncSemaphoreHandle);

  //  result = gettimeofday(&now, NULL);
  ////  ESP_LOGI(TAG, "time of day: %d", (int32_t)now.tv_sec +
  ///(int32_t)now.tv_usec);
  //  if (result) {
  //    ESP_LOGI(TAG, "Failed to gettimeofday");
  //
  //    return;
  //  }

  uint8_t *p_pkt = (uint8_t *)malloc(BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);
  if (p_pkt == NULL) {
    ESP_LOGW(
        TAG,
        "%s: Failed to get memory for time sync message. Skipping this round.",
        __func__);

    return;
  }

  memset(p_pkt, 0, BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);

  base_message_tx.type = SNAPCAST_MESSAGE_TIME;
  base_message_tx.id = id_counter++;
  base_message_tx.refersTo = 0;
  base_message_tx.received.sec = 0;
  base_message_tx.received.usec = 0;
  now = esp_timer_get_time();
  base_message_tx.sent.sec = now / 1000000;
  base_message_tx.sent.usec = now - base_message_tx.sent.sec * 1000000;
  base_message_tx.size = TIME_MESSAGE_SIZE;
  rc1 = base_message_serialize(&base_message_tx, (char *)&p_pkt[0],
                               BASE_MESSAGE_SIZE);
  if (rc1) {
    ESP_LOGE(TAG, "Failed to serialize base message for time");

    return;
  }

  //  memset(&time_message_tx, 0, sizeof(time_message_tx));
  //  result = time_message_serialize(&time_message_tx,
  //  &p_pkt[BASE_MESSAGE_SIZE],
  //                                  TIME_MESSAGE_SIZE);
  //  if (result) {
  //    ESP_LOGI(TAG, "Failed to serialize time message");
  //
  //    return;
  //  }

  rc1 = netconn_write(lwipNetconn, p_pkt, BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE,
                      NETCONN_NOCOPY);
  if (rc1 != ERR_OK) {
    ESP_LOGW(TAG, "error writing timesync msg");

    return;
  }

  free(p_pkt);

  //  ESP_LOGI(TAG, "%s: sent time sync message", __func__);

  //  xSemaphoreGiveFromISR(timeSyncSemaphoreHandle, &xHigherPriorityTaskWoken);
  //  if (xHigherPriorityTaskWoken) {
  //    portYIELD_FROM_ISR();
  //  }
}

/**
 *
 */
static FLAC__StreamDecoderReadStatus read_callback(
    const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes,
    void *client_data) {
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;
  //  decoderData_t *flacData;

  (void)scSet;

  // xQueueReceive(decoderReadQHdl, &flacData, portMAX_DELAY);
  // if (xQueueReceive(decoderReadQHdl, &flacData, pdMS_TO_TICKS(100)))
  if (decoderChunk.inData) {
    //	   ESP_LOGI(TAG, "in flac read cb %ld %p", flacData->bytes,
    // flacData->inData);

    if (decoderChunk.bytes <= 0) {
      //	    free_flac_data(flacData);

      return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

    isCachedChunk = false;

    //	  if (flacData->inData == NULL) {
    //	    free_flac_data(flacData);
    //
    //	    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    //	  }

    if (decoderChunk.bytes <= *bytes) {
      memcpy(buffer, decoderChunk.inData, decoderChunk.bytes);
      *bytes = decoderChunk.bytes;

      // ESP_LOGW(TAG, "read all flac inData %d", *bytes);

      free(decoderChunk.inData);
      decoderChunk.inData = NULL;
      decoderChunk.bytes = 0;
    } else {
      memcpy(buffer, decoderChunk.inData, *bytes);

      memmove(decoderChunk.inData, decoderChunk.inData + *bytes,
              decoderChunk.bytes - *bytes);
      decoderChunk.bytes -= *bytes;
      decoderChunk.inData =
          (uint8_t *)realloc(decoderChunk.inData, decoderChunk.bytes);

      // ESP_LOGW(TAG, "didn't read all flac inData %d", *bytes);
      //	    flacData->inData += *bytes;
      //	    flacData->bytes -= *bytes;
    }

    // free_flac_data(flacData);

    // xQueueSend (flacReadQHdl, &flacData, portMAX_DELAY);

    // xSemaphoreGive(decoderReadSemaphore);

    // ESP_LOGE(TAG, "%s: data processed", __func__);

    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
  } else {
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }
}

/**
 *
 */
static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data) {
  size_t i;
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;

  size_t bytes = frame->header.blocksize * frame->header.channels *
                 frame->header.bits_per_sample / 8;

  (void)decoder;

  if (isCachedChunk) {
    cachedBlocks += frame->header.blocksize;
  }

  //  ESP_LOGI(TAG, "in flac write cb %ld %d, pcmChunk.bytes %ld",
  //  frame->header.blocksize, bytes, pcmChunk.bytes);

  if (frame->header.channels != scSet->ch) {
    ESP_LOGE(TAG,
             "ERROR: frame header reports different channel count %ld than "
             "previous metadata block %d",
             frame->header.channels, scSet->ch);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (frame->header.bits_per_sample != scSet->bits) {
    ESP_LOGE(TAG,
             "ERROR: frame header reports different bps %ld than previous "
             "metadata block %d",
             frame->header.bits_per_sample, scSet->bits);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (buffer[0] == NULL) {
    ESP_LOGE(TAG, "ERROR: buffer [0] is NULL");
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (buffer[1] == NULL) {
    ESP_LOGE(TAG, "ERROR: buffer [1] is NULL");
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  pcmChunk.outData =
      (uint8_t *)realloc(pcmChunk.outData, pcmChunk.bytes + bytes);
  if (!pcmChunk.outData) {
    ESP_LOGE(TAG, "%s, failed to allocate PCM chunk payload", __func__);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  for (i = 0; i < frame->header.blocksize; i++) {
    // write little endian
    pcmChunk.outData[pcmChunk.bytes + 4 * i] = (uint8_t)(buffer[0][i]);
    pcmChunk.outData[pcmChunk.bytes + 4 * i + 1] = (uint8_t)(buffer[0][i] >> 8);
    pcmChunk.outData[pcmChunk.bytes + 4 * i + 2] = (uint8_t)(buffer[1][i]);
    pcmChunk.outData[pcmChunk.bytes + 4 * i + 3] = (uint8_t)(buffer[1][i] >> 8);
  }

  pcmChunk.bytes += bytes;

  scSet->chkInFrames = frame->header.blocksize;

  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

/**
 *
 */
void metadata_callback(const FLAC__StreamDecoder *decoder,
                       const FLAC__StreamMetadata *metadata,
                       void *client_data) {
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;

  (void)decoder;

  if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
    // ESP_LOGI(TAG, "in flac meta cb");

    // save for later
    scSet->sr = metadata->data.stream_info.sample_rate;
    scSet->ch = metadata->data.stream_info.channels;
    scSet->bits = metadata->data.stream_info.bits_per_sample;

    ESP_LOGI(TAG, "fLaC sampleformat: %ld:%d:%d", scSet->sr, scSet->bits,
             scSet->ch);

    // ESP_LOGE(TAG, "%s: data processed", __func__);
  }
}

/**
 *
 */
void error_callback(const FLAC__StreamDecoder *decoder,
                    FLAC__StreamDecoderErrorStatus status, void *client_data) {
  (void)decoder, (void)client_data;

  ESP_LOGE(TAG, "Got error callback: %s\n",
           FLAC__StreamDecoderErrorStatusString[status]);
}

/**
 *
 */
void init_snapcast(QueueHandle_t audioQHdl) {
  audioDACQHdl = audioQHdl;
  audioDACSemaphore = xSemaphoreCreateMutex();
  audioDAC_data.mute = true;
  audioDAC_data.volume = -1; // invalid volume to force update on first set
}

/**
 *
 */
void audio_set_mute(bool mute) {
  xSemaphoreTake(audioDACSemaphore, portMAX_DELAY);
  if (mute != audioDAC_data.mute) {
    audioDAC_data.mute = mute;
    xQueueOverwrite(audioDACQHdl, &audioDAC_data);
  }
  xSemaphoreGive(audioDACSemaphore);
}

/**
 *
 */
void audio_set_volume(int volume) {
  xSemaphoreTake(audioDACSemaphore, portMAX_DELAY);
  if (volume != audioDAC_data.volume) {
    audioDAC_data.volume = volume;
    xQueueOverwrite(audioDACQHdl, &audioDAC_data);
  }
  xSemaphoreGive(audioDACSemaphore);
  
#if CONFIG_ENABLE_SH1106_DISPLAY
  // Update display with new snapcast volume
  display_set_snapcast_volume((uint8_t)volume);
#endif
}

// Static variable to store volume before Bluetooth mute
static int pre_bluetooth_volume = -1;

/**
 * Mute Snapcast when Bluetooth audio starts playing
 */
void snapcast_mute_for_bluetooth(void) {
    // Store current volume if not already muted
    if (pre_bluetooth_volume == -1 && scSet.volume > 0) {
        pre_bluetooth_volume = scSet.volume;
        ESP_LOGI("SC", "Storing Snapcast volume (%d%%) and muting for Bluetooth", pre_bluetooth_volume);
    }
    
    // Set volume to 0 to mute Snapcast
    if (scSet.volume > 0) {
        scSet.volume = 0;
        scSet.muted = true;
        audio_set_volume(0);
      // Inform Snapserver so it stops sending audio and frees WiFi
      send_volume_update_to_server(0);
#if CONFIG_ENABLE_SH1106_DISPLAY
        display_set_snapcast_mute(true);
#endif
        ESP_LOGI("SC", "Snapcast muted for Bluetooth audio");
    }
}

/**
 * Unmute Snapcast when Bluetooth audio stops
 */
void snapcast_unmute_after_bluetooth(void) {
    // Restore previous volume if we have one stored
    if (pre_bluetooth_volume > 0) {
        ESP_LOGI("SC", "Restoring Snapcast volume to %d%% after Bluetooth", pre_bluetooth_volume);
        scSet.volume = pre_bluetooth_volume;
        scSet.muted = false;
        audio_set_volume(pre_bluetooth_volume);
        // Restore volume on Snapserver so stream resumes at the same level
        send_volume_update_to_server(pre_bluetooth_volume);
#if CONFIG_ENABLE_SH1106_DISPLAY
        display_set_snapcast_mute(false);
#endif
        pre_bluetooth_volume = -1; // Reset stored volume
        ESP_LOGI("SC", "Snapcast unmuted and restored to %"PRIu32"%%", scSet.volume);
    } else {
        ESP_LOGW("SC", "No previous Snapcast volume to restore");
    }
}

/**
 *
 */
static void http_get_task(void *pvParameters) {
  char *start;
  base_message_t base_message_rx;
  hello_message_t hello_message;
  wire_chunk_message_t wire_chnk = {{0, 0}, 0, NULL};
  char *hello_message_serialized = NULL;
  int result;
  int64_t now, trx, tdif, ttx;
  time_message_t time_message_rx = {{0, 0}};
  int64_t tmpDiffToServer;
  int64_t lastTimeSync = 0;
  esp_timer_handle_t timeSyncMessageTimer = NULL;
  server_settings_message_t server_settings_message;
  bool received_header = false;
  codec_type_t codec = NONE;
  extern snapcastSetting_t scSet;  // Use global scSet instead of local shadow
  pcm_chunk_message_t *pcmData = NULL;
  uint16_t remotePort = 0;
  int rc1 = ERR_OK, rc2 = ERR_OK;
  struct netbuf *firstNetBuf = NULL;
  uint16_t len;
  uint64_t timeout = FAST_SYNC_LATENCY_BUF;
  char *codecString = NULL;
  char *codecPayload = NULL;
  char *serverSettingsString = NULL;

  // create a timer to send time sync messages every x µs
  esp_timer_create(&tSyncArgs, &timeSyncMessageTimer);

#if CONFIG_SNAPCLIENT_USE_MDNS
  ESP_LOGI(TAG, "Enable mdns");
  mdns_init();
#endif

  while (1) {
    // do some house keeping
    {
      received_header = false;

      timeout = FAST_SYNC_LATENCY_BUF;

      esp_timer_stop(timeSyncMessageTimer);

      if (opusDecoder != NULL) {
        opus_decoder_destroy(opusDecoder);
        opusDecoder = NULL;
      }

      if (flacDecoder != NULL) {
        FLAC__stream_decoder_finish(flacDecoder);
        FLAC__stream_decoder_delete(flacDecoder);
        flacDecoder = NULL;
      }

      if (decoderChunk.inData) {
        free(decoderChunk.inData);
        decoderChunk.inData = NULL;
      }

      if (decoderChunk.outData) {
        free(decoderChunk.outData);
        decoderChunk.outData = NULL;
      }

      if (codecString) {
        free(codecString);
        codecString = NULL;
      }

      if (codecPayload) {
        free(codecPayload);
        codecPayload = NULL;
      }

      if (codecPayload) {
        free(serverSettingsString);
        serverSettingsString = NULL;
      }
    }

#if SNAPCAST_SERVER_USE_MDNS
    // Find snapcast server
    // Connect to first snapcast server found
    mdns_result_t *r = NULL;
    esp_err_t err = ESP_OK;
    while (!r || err != ESP_OK) {
      ESP_LOGI(TAG, "Lookup snapcast service on network");
      err = mdns_query_ptr("_snapcast", "_tcp", 3000, 20, &r);
      if (err) {
        ESP_LOGE(TAG, "Query Failed");
        vTaskDelay(pdMS_TO_TICKS(1000));
      }

      if (!r) {
        ESP_LOGW(TAG, "No results found!");
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    mdns_ip_addr_t *a = r->addr;
    if (a) {
      ip_addr_copy(remote_ip, (a->addr));
      remote_ip.type = a->addr.type;
      remotePort = r->port;
      ESP_LOGI(TAG, "Found %s:%d", ipaddr_ntoa(&remote_ip), remotePort);

      mdns_query_results_free(r);
    } else {
      mdns_query_results_free(r);

      ESP_LOGW(TAG, "No IP found in MDNS query");

      continue;
    }
#else
    // configure a failsafe snapserver according to CONFIG values
    struct sockaddr_in servaddr;

    const char *snap_host = SNAPCAST_SERVER_HOST;
    int snap_port = SNAPCAST_SERVER_PORT;

    if (g_system_config.snapserver_host[0] != '\0') {
      snap_host = g_system_config.snapserver_host;
    }
    if (g_system_config.snapserver_port > 0 && g_system_config.snapserver_port <= 65535) {
      snap_port = g_system_config.snapserver_port;
    }

    servaddr.sin_family = AF_INET;
    inet_pton(AF_INET, snap_host, &(servaddr.sin_addr.s_addr));
    servaddr.sin_port = htons(snap_port);

    inet_pton(AF_INET, snap_host, &(remote_ip.u_addr.ip4.addr));
    remote_ip.type = IPADDR_TYPE_V4;
    remotePort = snap_port;

    ESP_LOGI(TAG, "try connecting to static configuration %s:%d",
             ipaddr_ntoa(&remote_ip), remotePort);
#endif

    if (lwipNetconn != NULL) {
      netconn_delete(lwipNetconn);
      lwipNetconn = NULL;
    }

    lwipNetconn = netconn_new(NETCONN_TCP);
    if (lwipNetconn == NULL) {
      ESP_LOGE(TAG, "can't create netconn");

      continue;
    }

    rc1 = netconn_bind(lwipNetconn, IPADDR_ANY, 0);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "can't bind local IP");
    }

    rc2 = netconn_connect(lwipNetconn, &remote_ip, remotePort);
    if (rc2 != ERR_OK) {
      ESP_LOGE(TAG, "can't connect to remote %s:%d, err %d",
               ipaddr_ntoa(&remote_ip), remotePort, rc2);
    }

    if (rc1 != ERR_OK || rc2 != ERR_OK) {
      netconn_close(lwipNetconn);
      netconn_delete(lwipNetconn);
      lwipNetconn = NULL;

      continue;
    }

    ESP_LOGI(TAG, "netconn connected");

    if (reset_latency_buffer() < 0) {
      ESP_LOGE(TAG,
               "reset_diff_buffer: couldn't reset median filter long. STOP");
      return;
    }

    char mac_address[18];
    uint8_t base_mac[6];
    // Get MAC address for WiFi station
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    esp_read_mac(base_mac, ESP_MAC_ETH);
#else
    esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
#endif
    sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0],
            base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);

    now = esp_timer_get_time();

    // init base message
    base_message_rx.type = SNAPCAST_MESSAGE_HELLO;
    base_message_rx.id = id_counter++;
    base_message_rx.refersTo = 0x0000;
    base_message_rx.sent.sec = now / 1000000;
    base_message_rx.sent.usec = now - base_message_rx.sent.sec * 1000000;
    base_message_rx.received.sec = 0;
    base_message_rx.received.usec = 0;
    base_message_rx.size = 0x00000000;

    // init hello message
    hello_message.mac = mac_address;
    // Use runtime-configured snapclient name for Snapcast hello
    hello_message.hostname = g_system_config.snapclient_name;
    hello_message.version = (char *)VERSION_STRING;
    hello_message.client_name = "libsnapcast";
    hello_message.os = "esp32";
    hello_message.arch = "xtensa";
    hello_message.instance = 1;
    hello_message.id = mac_address;
    hello_message.protocol_version = 2;

    if (hello_message_serialized == NULL) {
      hello_message_serialized = hello_message_serialize(
          &hello_message, (size_t *)&(base_message_rx.size));
      if (!hello_message_serialized) {
        ESP_LOGE(TAG, "Failed to serialize hello message");
        return;
      }
    }

    result = base_message_serialize(&base_message_rx, base_message_serialized,
                                    BASE_MESSAGE_SIZE);
    if (result) {
      ESP_LOGE(TAG, "Failed to serialize base message");
      return;
    }

    rc1 = netconn_write(lwipNetconn, base_message_serialized, BASE_MESSAGE_SIZE,
                        NETCONN_NOCOPY);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "netconn failed to send base message");

      continue;
    }
    rc1 = netconn_write(lwipNetconn, hello_message_serialized,
                        base_message_rx.size, NETCONN_NOCOPY);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "netconn failed to send hello message");

      continue;
    }

    ESP_LOGI(TAG, "netconn sent hello message");

    free(hello_message_serialized);
    hello_message_serialized = NULL;

    // init default setting
    scSet.buf_ms = 500;
    scSet.codec = NONE;
    scSet.bits = 16;
    scSet.ch = 2;
    scSet.sr = 44100;
    scSet.chkInFrames = 0;
    scSet.volume = 0;
    scSet.muted = true;

    //    size_t currentPos = 0;
    size_t typedMsgCurrentPos = 0;
    uint32_t typedMsgLen = 0;
    uint32_t offset = 0;
    uint32_t payloadOffset = 0;
    uint32_t tmpData = 0;
    int32_t payloadDataShift = 0;

#define BASE_MESSAGE_STATE 0
#define TYPED_MESSAGE_STATE 1

    // 0 ... base message, 1 ... typed message
    uint32_t state = BASE_MESSAGE_STATE;
    uint32_t internalState = 0;

    firstNetBuf = NULL;

    while (1) {
      rc2 = netconn_recv(lwipNetconn, &firstNetBuf);
      if (rc2 != ERR_OK) {
        if (rc2 == ERR_CONN) {
          netconn_close(lwipNetconn);

          // restart and try to reconnect
          break;
        }

        if (firstNetBuf != NULL) {
          netbuf_delete(firstNetBuf);

          firstNetBuf = NULL;
        }
        continue;
      }

      // now parse the data
      netbuf_first(firstNetBuf);
      do {
        // currentPos = 0;

        rc1 = netbuf_data(firstNetBuf, (void **)&start, &len);
        if (rc1 == ERR_OK) {
          // ESP_LOGI (TAG, "netconn rx,"
          // "data len: %d, %d", len, netbuf_len(firstNetBuf) -
          // currentPos);
        } else {
          ESP_LOGE(TAG, "netconn rx, couldn't get data");

          continue;
        }

        while (len > 0) {
          rc1 = ERR_OK;  // probably not necessary

          switch (state) {
            // decode base message
            case BASE_MESSAGE_STATE: {
              switch (internalState) {
                case 0:
                  base_message_rx.type = *start & 0xFF;
                  internalState++;
                  break;

                case 1:
                  base_message_rx.type |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 2:
                  base_message_rx.id = *start & 0xFF;
                  internalState++;
                  break;

                case 3:
                  base_message_rx.id |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 4:
                  base_message_rx.refersTo = *start & 0xFF;
                  internalState++;
                  break;

                case 5:
                  base_message_rx.refersTo |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 6:
                  base_message_rx.sent.sec = *start & 0xFF;
                  internalState++;
                  break;

                case 7:
                  base_message_rx.sent.sec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 8:
                  base_message_rx.sent.sec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 9:
                  base_message_rx.sent.sec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 10:
                  base_message_rx.sent.usec = *start & 0xFF;
                  internalState++;
                  break;

                case 11:
                  base_message_rx.sent.usec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 12:
                  base_message_rx.sent.usec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 13:
                  base_message_rx.sent.usec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 14:
                  base_message_rx.received.sec = *start & 0xFF;
                  internalState++;
                  break;

                case 15:
                  base_message_rx.received.sec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 16:
                  base_message_rx.received.sec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 17:
                  base_message_rx.received.sec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 18:
                  base_message_rx.received.usec = *start & 0xFF;
                  internalState++;
                  break;

                case 19:
                  base_message_rx.received.usec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 20:
                  base_message_rx.received.usec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 21:
                  base_message_rx.received.usec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 22:
                  base_message_rx.size = *start & 0xFF;
                  internalState++;
                  break;

                case 23:
                  base_message_rx.size |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 24:
                  base_message_rx.size |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 25:
                  base_message_rx.size |= (*start & 0xFF) << 24;
                  internalState = 0;

                  now = esp_timer_get_time();

                  base_message_rx.received.sec = now / 1000000;
                  base_message_rx.received.usec =
                      now - base_message_rx.received.sec * 1000000;

                  typedMsgCurrentPos = 0;

                  //                   ESP_LOGI(TAG,"BM type %d ts %d.%d",
                  //                   base_message_rx.type,
                  //                   base_message_rx.received.sec,
                  //                   base_message_rx.received.usec);
                  // ESP_LOGI(TAG,"%d, %d.%d", base_message_rx.type,
                  //                   base_message_rx.received.sec,
                  //                   base_message_rx.received.usec);
                  // ESP_LOGI(TAG,"%d, %llu", base_message_rx.type,
                  //		   1000000ULL * base_message_rx.received.sec +
                  // base_message_rx.received.usec);

                  state = TYPED_MESSAGE_STATE;
                  break;
              }

              // currentPos++;++;
              len--;
              start++;

              break;
            }

            // decode typed message
            case TYPED_MESSAGE_STATE: {
              switch (base_message_rx.type) {
                case SNAPCAST_MESSAGE_WIRE_CHUNK: {
                  switch (internalState) {
                    case 0: {
                      wire_chnk.timestamp.sec = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 1: {
                      wire_chnk.timestamp.sec |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 2: {
                      wire_chnk.timestamp.sec |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 3: {
                      wire_chnk.timestamp.sec |= (*start & 0xFF) << 24;

                      // ESP_LOGI(TAG,
                      // "wire chunk time sec: %d",
                      // wire_chnk.timestamp.sec);

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 4: {
                      wire_chnk.timestamp.usec = (*start & 0xFF);

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 5: {
                      wire_chnk.timestamp.usec |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 6: {
                      wire_chnk.timestamp.usec |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 7: {
                      wire_chnk.timestamp.usec |= (*start & 0xFF) << 24;

                      // ESP_LOGI(TAG,
                      // "wire chunk time usec: %d",
                      // wire_chnk.timestamp.usec);

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 8: {
                      wire_chnk.size = (*start & 0xFF);

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 9: {
                      wire_chnk.size |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 10: {
                      wire_chnk.size |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 11: {
                      wire_chnk.size |= (*start & 0xFF) << 24;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      // TODO: we could use wire chunk directly maybe?
                      decoderChunk.bytes = wire_chnk.size;
                      while (!decoderChunk.inData) {
                        decoderChunk.inData =
                            (uint8_t *)malloc(decoderChunk.bytes);
                        if (!decoderChunk.inData) {
                          ESP_LOGW(TAG,
                                   "malloc decoderChunk.inData failed, wait "
                                   "1ms and try again");

                          vTaskDelay(pdMS_TO_TICKS(1));
                        }
                      }

                      payloadOffset = 0;

#if 0
                       ESP_LOGI(TAG, "chunk with size: %u, at time %ld.%ld",
                    		   	   	   	   	 wire_chnk.size,
                                             wire_chnk.timestamp.sec,
                                             wire_chnk.timestamp.usec);
#endif

                      if (len == 0) {
                        break;
                      }
                    }

                    case 12: {
                      size_t tmp_size;

                      if ((base_message_rx.size - typedMsgCurrentPos) <= len) {
                        tmp_size = base_message_rx.size - typedMsgCurrentPos;
                      } else {
                        tmp_size = len;
                      }

                      if (received_header == true) {
                        switch (codec) {
                          case OPUS:
                          case FLAC: {
                            memcpy(&decoderChunk.inData[payloadOffset], start,
                                   tmp_size);
                            payloadOffset += tmp_size;
                            decoderChunk.outData = NULL;
                            decoderChunk.type = SNAPCAST_MESSAGE_WIRE_CHUNK;

                            break;
                          }

                          case PCM: {
                            size_t _tmp = tmp_size;

                            offset = 0;

                            if (pcmData == NULL) {
                              if (allocate_pcm_chunk_memory(
                                      &pcmData, wire_chnk.size) < 0) {
                                pcmData = NULL;
                              }

                              tmpData = 0;
                              payloadDataShift = 3;
                              payloadOffset = 0;
                            }

                            while (_tmp--) {
                              tmpData |= ((uint32_t)start[offset++]
                                          << (8 * payloadDataShift));

                              payloadDataShift--;
                              if (payloadDataShift < 0) {
                                payloadDataShift = 3;

                                if ((pcmData) && (pcmData->fragment->payload)) {
                                  volatile uint32_t *sample;
                                  uint8_t dummy1;
                                  uint32_t dummy2 = 0;

                                  // TODO: find a more
                                  // clever way to do this,
                                  // best would be to
                                  // actually store it the
                                  // right way in the first
                                  // place
                                  dummy1 = tmpData >> 24;
                                  dummy2 |= (uint32_t)dummy1 << 16;
                                  dummy1 = tmpData >> 16;
                                  dummy2 |= (uint32_t)dummy1 << 24;
                                  dummy1 = tmpData >> 8;
                                  dummy2 |= (uint32_t)dummy1 << 0;
                                  dummy1 = tmpData >> 0;
                                  dummy2 |= (uint32_t)dummy1 << 8;
                                  tmpData = dummy2;

                                  sample = (volatile uint32_t *)(&(
                                      pcmData->fragment
                                          ->payload[payloadOffset]));
                                  *sample = (volatile uint32_t)tmpData;

                                  payloadOffset += 4;
                                }

                                tmpData = 0;
                              }
                            }

                            break;
                          }

                          default: {
                            ESP_LOGE(TAG, "Decoder (1) not supported");

                            return;

                            break;
                          }
                        }
                      }

                      typedMsgCurrentPos += tmp_size;
                      start += tmp_size;
                      // currentPos += tmp_size;
                      len -= tmp_size;

                      if (typedMsgCurrentPos >= base_message_rx.size) {
                        if (received_header == true) {
                          switch (codec) {
                            case OPUS: {
                              int frame_size = -1;
                              int samples_per_frame;
                              opus_int16 *audio = NULL;

                              samples_per_frame =
                                  opus_packet_get_samples_per_frame(
                                      decoderChunk.inData, scSet.sr);
                              if (samples_per_frame < 0) {
                                ESP_LOGE(TAG,
                                         "couldn't get samples per frame count "
                                         "of packet");
                              }

                              scSet.chkInFrames = samples_per_frame;

                              // ESP_LOGW(TAG, "%d, %llu, %llu",
                              // samples_per_frame, 1000000ULL *
                              // samples_per_frame / scSet.sr,
                              // 1000000ULL *
                              // wire_chnk.timestamp.sec +
                              // wire_chnk.timestamp.usec);

                              // ESP_LOGW(TAG, "got OPUS decoded chunk size: %ld
                              // " "frames from encoded chunk with size %d,
                              // allocated audio buffer %d", scSet.chkInFrames,
                              // wire_chnk.size, samples_per_frame);

                              size_t bytes;
                              do {
                                bytes = samples_per_frame *
                                        (scSet.ch * scSet.bits >> 3);

                                while ((audio = (opus_int16 *)realloc(
                                            audio, bytes)) == NULL) {
                                  ESP_LOGE(TAG,
                                           "couldn't realloc memory for OPUS "
                                           "audio %d",
                                           bytes);

                                  vTaskDelay(pdMS_TO_TICKS(1));
                                }

                                frame_size = opus_decode(
                                    opusDecoder, decoderChunk.inData,
                                    decoderChunk.bytes, (opus_int16 *)audio,
                                    samples_per_frame, 0);

                                samples_per_frame <<= 1;
                              } while (frame_size < 0);

                              free(decoderChunk.inData);
                              decoderChunk.inData = NULL;

                              pcm_chunk_message_t *new_pcmChunk = NULL;

                              // ESP_LOGW(TAG, "OPUS decode: %d", frame_size);

                              if (allocate_pcm_chunk_memory(&new_pcmChunk,
                                                            bytes) < 0) {
                                pcmData = NULL;
                              } else {
                                new_pcmChunk->timestamp = wire_chnk.timestamp;

                                if (new_pcmChunk->fragment->payload) {
                                  volatile uint32_t *sample;
                                  uint32_t tmpData;
                                  uint32_t cnt = 0;

                                  for (int i = 0; i < bytes; i += 4) {
                                    sample = (volatile uint32_t *)(&(
                                        new_pcmChunk->fragment->payload[i]));
                                    tmpData = (((uint32_t)audio[cnt] << 16) &
                                               0xFFFF0000) |
                                              (((uint32_t)audio[cnt + 1] << 0) &
                                               0x0000FFFF);
                                    *sample = (volatile uint32_t)tmpData;

                                    cnt += 2;
                                  }
                                }

                                free(audio);
                                audio = NULL;

#ifdef CONFIG_ENABLE_LED_CONTROLLER
                                bool ignore_volume_for_led = false;
                                if (led_initialized && new_pcmChunk && new_pcmChunk->fragment->payload) {
                                  led_config_t led_cfg;
                                  if (led_controller_get_config(&led_cfg) == ESP_OK) {
                                    ignore_volume_for_led = (led_cfg.ignore_volume != 0);
                                  }
                                }
#endif

#if CONFIG_USE_DSP_PROCESSOR
                                if (new_pcmChunk->fragment->payload) {
#ifdef CONFIG_ENABLE_LED_CONTROLLER
                                  if (ignore_volume_for_led) {
                                    // Feed LEDs with pre-volume PCM when ignoring playback volume
                                    led_controller_feed_audio(
                                        (const int16_t *)new_pcmChunk->fragment->payload,
                                        new_pcmChunk->fragment->size / 4);
                                  }
#endif

                                  dsp_processor_worker(
                                      new_pcmChunk->fragment->payload,
                                      new_pcmChunk->fragment->size, scSet.sr);
                                }
#endif

#ifdef CONFIG_ENABLE_LED_CONTROLLER
                                if (led_initialized && new_pcmChunk && new_pcmChunk->fragment->payload) {
#if CONFIG_USE_DSP_PROCESSOR
                                  if (!ignore_volume_for_led) {
                                    // Normal path: LEDs follow post-volume audio
                                    led_controller_feed_audio(
                                        (const int16_t *)new_pcmChunk->fragment->payload,
                                        new_pcmChunk->fragment->size / 4);
                                  }
#else
                                  // No DSP/soft volume: always feed as-is
                                  led_controller_feed_audio(
                                      (const int16_t *)new_pcmChunk->fragment->payload,
                                      new_pcmChunk->fragment->size / 4);
#endif
                                }
#endif

                                insert_pcm_chunk(new_pcmChunk);
                              }

                              if (player_send_snapcast_setting(&scSet) !=
                                  pdPASS) {
                                ESP_LOGE(TAG,
                                         "Failed to notify "
                                         "sync task about "
                                         "codec. Did you "
                                         "init player?");

                                return;
                              }

                              break;
                            }

                            case FLAC: {
                              isCachedChunk = true;
                              cachedBlocks = 0;

                              while (decoderChunk.bytes > 0) {
                                if (FLAC__stream_decoder_process_single(
                                        flacDecoder) == 0) {
                                  ESP_LOGE(
                                      TAG,
                                      "%s: FLAC__stream_decoder_process_single "
                                      "failed",
                                      __func__);

                                  // TODO: should insert some abort condition?
                                  vTaskDelay(pdMS_TO_TICKS(10));
                                }
                              }

                              // alternating chunk sizes need time stamp repair
                              if ((cachedBlocks > 0) && (scSet.sr != 0)) {
                                uint64_t diffUs =
                                    1000000ULL * cachedBlocks / scSet.sr;

                                uint64_t timestamp =
                                    1000000ULL * wire_chnk.timestamp.sec +
                                    wire_chnk.timestamp.usec;

                                timestamp = timestamp - diffUs;

                                wire_chnk.timestamp.sec =
                                    timestamp / 1000000ULL;
                                wire_chnk.timestamp.usec =
                                    timestamp % 1000000ULL;
                              }

                              pcm_chunk_message_t *new_pcmChunk;
                              int32_t ret = allocate_pcm_chunk_memory(
                                  &new_pcmChunk, pcmChunk.bytes);

                              scSet.chkInFrames =
                                  FLAC__stream_decoder_get_blocksize(
                                      flacDecoder);

                              // ESP_LOGE (TAG, "block size: %ld",
                              // scSet.chkInFrames * scSet.bits / 8 * scSet.ch);
                              // ESP_LOGI(TAG, "new_pcmChunk with size %ld",
                              // new_pcmChunk->totalSize);

                              if (ret == 0) {
                                pcm_chunk_fragment_t *fragment =
                                    new_pcmChunk->fragment;
                                uint32_t fragmentCnt = 0;

                                if (fragment->payload != NULL) {
                                  uint32_t frames =
                                      pcmChunk.bytes /
                                      (scSet.ch * (scSet.bits / 8));

                                  for (int i = 0; i < frames; i++) {
                                    // TODO: for now fragmented payload is not
                                    // supported and the whole chunk is expected
                                    // to be in the first fragment
                                    uint32_t tmpData;
                                    memcpy(&tmpData,
                                           &pcmChunk.outData[fragmentCnt],
                                           (scSet.ch * (scSet.bits / 8)));

                                    if (fragment != NULL) {
                                      volatile uint32_t *test =
                                          (volatile uint32_t *)(&(
                                              fragment->payload[fragmentCnt]));
                                      *test = (volatile uint32_t)tmpData;
                                    }

                                    fragmentCnt +=
                                        (scSet.ch * (scSet.bits / 8));
                                    if (fragmentCnt >= fragment->size) {
                                      fragmentCnt = 0;

                                      fragment = fragment->nextFragment;
                                    }
                                  }
                                }

                                new_pcmChunk->timestamp = wire_chnk.timestamp;

#ifdef CONFIG_ENABLE_LED_CONTROLLER
                                bool ignore_volume_for_led = false;
                                if (led_initialized && new_pcmChunk && new_pcmChunk->fragment->payload) {
                                  led_config_t led_cfg;
                                  if (led_controller_get_config(&led_cfg) == ESP_OK) {
                                    ignore_volume_for_led = (led_cfg.ignore_volume != 0);
                                  }
                                }
#endif

#if CONFIG_USE_DSP_PROCESSOR
                                if (new_pcmChunk->fragment->payload) {
#ifdef CONFIG_ENABLE_LED_CONTROLLER
                                  if (ignore_volume_for_led) {
                                    // Feed LEDs with pre-volume PCM when ignoring playback volume
                                    led_controller_feed_audio(
                                        (const int16_t *)new_pcmChunk->fragment->payload,
                                        new_pcmChunk->fragment->size / 4);
                                  }
#endif

                                  dsp_processor_worker(
                                      new_pcmChunk->fragment->payload,
                                      new_pcmChunk->fragment->size, scSet.sr);
                                }

#endif

#ifdef CONFIG_ENABLE_LED_CONTROLLER
                                if (led_initialized && new_pcmChunk && new_pcmChunk->fragment->payload) {
#if CONFIG_USE_DSP_PROCESSOR
                                  if (!ignore_volume_for_led) {
                                    // Normal path: LEDs follow post-volume audio
                                    led_controller_feed_audio(
                                        (const int16_t *)new_pcmChunk->fragment->payload,
                                        new_pcmChunk->fragment->size / 4);
                                  }
#else
                                  // No DSP/soft volume: always feed as-is
                                  led_controller_feed_audio(
                                      (const int16_t *)new_pcmChunk->fragment->payload,
                                      new_pcmChunk->fragment->size / 4);
#endif
                                }
#endif

                                insert_pcm_chunk(new_pcmChunk);
                              }

                              free(pcmChunk.outData);
                              pcmChunk.outData = NULL;
                              pcmChunk.bytes = 0;

                              if (player_send_snapcast_setting(&scSet) !=
                                  pdPASS) {
                                ESP_LOGE(TAG,
                                         "Failed to "
                                         "notify "
                                         "sync task "
                                         "about "
                                         "codec. Did you "
                                         "init player?");

                                return;
                              }

                              break;
                            }

                            case PCM: {
                              size_t decodedSize = wire_chnk.size;

                              // ESP_LOGW(TAG, "got PCM chunk,"
                              //               "typedMsgCurrentPos %d",
                              //               typedMsgCurrentPos);

                              if (pcmData) {
                                pcmData->timestamp = wire_chnk.timestamp;
                              }

                              scSet.chkInFrames =
                                  decodedSize /
                                  ((size_t)scSet.ch * (size_t)(scSet.bits / 8));

                              // ESP_LOGW(TAG,
                              //          "got PCM decoded chunk size: %ld
                              //          frames", scSet.chkInFrames);

                              if (player_send_snapcast_setting(&scSet) !=
                                  pdPASS) {
                                ESP_LOGE(TAG,
                                         "Failed to notify "
                                         "sync task about "
                                         "codec. Did you "
                                         "init player?");

                                return;
                              }

#ifdef CONFIG_ENABLE_LED_CONTROLLER
                              bool ignore_volume_for_led = false;
                              if (led_initialized && pcmData && pcmData->fragment->payload) {
                                led_config_t led_cfg;
                                if (led_controller_get_config(&led_cfg) == ESP_OK) {
                                  ignore_volume_for_led = (led_cfg.ignore_volume != 0);
                                }
                              }
#endif

#if CONFIG_USE_DSP_PROCESSOR
                              if ((pcmData) && (pcmData->fragment->payload)) {
#ifdef CONFIG_ENABLE_LED_CONTROLLER
                                if (ignore_volume_for_led) {
                                  // Feed LEDs with pre-volume PCM when ignoring playback volume
                                  led_controller_feed_audio(
                                      (const int16_t *)pcmData->fragment->payload,
                                      pcmData->fragment->size / 4);
                                }
#endif

                                dsp_processor_worker(pcmData->fragment->payload,
                                                     pcmData->fragment->size,
                                                     scSet.sr);
                              }
#endif

#ifdef CONFIG_ENABLE_LED_CONTROLLER
                              if (led_initialized && pcmData && pcmData->fragment->payload) {
#if CONFIG_USE_DSP_PROCESSOR
                                if (!ignore_volume_for_led) {
                                  // Normal path: LEDs follow post-volume audio
                                  led_controller_feed_audio(
                                      (const int16_t *)pcmData->fragment->payload,
                                      pcmData->fragment->size / 4);
                                }
#else
                                // No DSP/soft volume: always feed as-is
                                led_controller_feed_audio(
                                    (const int16_t *)pcmData->fragment->payload,
                                    pcmData->fragment->size / 4);
#endif
                              }
#endif

                              if (pcmData) {
                                insert_pcm_chunk(pcmData);
                              }

                              pcmData = NULL;

                              free(decoderChunk.inData);
                              decoderChunk.inData = NULL;

                              break;
                            }

                            default: {
                              ESP_LOGE(TAG,
                                       "Decoder (2) not "
                                       "supported");

                              return;

                              break;
                            }
                          }
                        }

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        typedMsgCurrentPos = 0;
                      }

                      break;
                    }

                    default: {
                      ESP_LOGE(TAG,
                               "wire chunk decoder "
                               "shouldn't get here");

                      break;
                    }
                  }

                  break;
                }

                case SNAPCAST_MESSAGE_CODEC_HEADER: {
                  switch (internalState) {
                    case 0: {
                      typedMsgLen = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 1: {
                      typedMsgLen |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 2: {
                      typedMsgLen |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 3: {
                      typedMsgLen |= (*start & 0xFF) << 24;

                      codecString =
                          malloc(typedMsgLen + 1);  // allocate memory for
                                                    // codec string
                      if (codecString == NULL) {
                        ESP_LOGE(TAG,
                                 "couldn't get memory "
                                 "for codec string");

                        return;
                      }

                      offset = 0;
                      // ESP_LOGI(TAG,
                      // "codec header string is %d long",
                      // typedMsgLen);

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 4: {
                      if (len >= typedMsgLen) {
                        memcpy(&codecString[offset], start, typedMsgLen);

                        offset += typedMsgLen;

                        typedMsgCurrentPos += typedMsgLen;
                        start += typedMsgLen;
                        // currentPos += typedMsgLen;
                        len -= typedMsgLen;
                      } else {
                        memcpy(&codecString[offset], start, typedMsgLen);

                        offset += len;

                        typedMsgCurrentPos += len;
                        start += len;
                        // currentPos += len;
                        len -= len;
                      }

                      if (offset == typedMsgLen) {
                        // NULL terminate string
                        codecString[typedMsgLen] = 0;

                        // ESP_LOGI (TAG, "got codec string: %s", tmp);

                        if (strcmp(codecString, "opus") == 0) {
                          codec = OPUS;
                        } else if (strcmp(codecString, "flac") == 0) {
                          codec = FLAC;
                        } else if (strcmp(codecString, "pcm") == 0) {
                          codec = PCM;
                        } else {
                          codec = NONE;

                          ESP_LOGI(TAG, "Codec : %s not supported",
                                   codecString);
                          ESP_LOGI(TAG,
                                   "Change encoder codec to "
                                   "opus, flac or pcm in "
                                   "/etc/snapserver.conf on "
                                   "server");

                          return;
                        }

                        free(codecString);
                        codecString = NULL;

                        internalState++;
                      }

                      if (len == 0) {
                        break;
                      }
                    }

                    case 5: {
                      typedMsgLen = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 6: {
                      typedMsgLen |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 7: {
                      typedMsgLen |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 8: {
                      typedMsgLen |= (*start & 0xFF) << 24;

                      codecPayload = malloc(typedMsgLen);  // allocate memory
                                                           // for codec payload
                      if (codecPayload == NULL) {
                        ESP_LOGE(TAG,
                                 "couldn't get memory "
                                 "for codec payload");

                        return;
                      }

                      offset = 0;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 9: {
                      if (len >= typedMsgLen) {
                        memcpy(&codecPayload[offset], start, typedMsgLen);

                        offset += typedMsgLen;

                        typedMsgCurrentPos += typedMsgLen;
                        start += typedMsgLen;
                        // currentPos += typedMsgLen;
                        len -= typedMsgLen;
                      } else {
                        memcpy(&codecPayload[offset], start, len);

                        offset += len;

                        typedMsgCurrentPos += len;
                        start += len;
                        // currentPos += len;
                        len -= len;
                      }

                      if (offset == typedMsgLen) {
                        // first ensure everything is set up
                        // correctly and resources are
                        // available

                        if (flacDecoder != NULL) {
                          FLAC__stream_decoder_finish(flacDecoder);
                          FLAC__stream_decoder_delete(flacDecoder);
                          flacDecoder = NULL;
                        }

                        if (opusDecoder != NULL) {
                          opus_decoder_destroy(opusDecoder);
                          opusDecoder = NULL;
                        }

                        if (codec == OPUS) {
                          uint16_t channels;
                          uint32_t rate;
                          uint16_t bits;

                          memcpy(&rate, codecPayload + 4, sizeof(rate));
                          memcpy(&bits, codecPayload + 8, sizeof(bits));
                          memcpy(&channels, codecPayload + 10,
                                 sizeof(channels));

                          scSet.codec = codec;
                          scSet.bits = bits;
                          scSet.ch = channels;
                          scSet.sr = rate;

                          ESP_LOGI(TAG, "Opus sample format: %ld:%d:%d\n", rate,
                                   bits, channels);

                          int error = 0;

                          opusDecoder =
                              opus_decoder_create(scSet.sr, scSet.ch, &error);
                          if (error != 0) {
                            ESP_LOGI(TAG, "Failed to init opus coder");
                            return;
                          }

                          ESP_LOGI(TAG, "Initialized opus Decoder: %d", error);
                        } else if (codec == FLAC) {
                          decoderChunk.bytes = typedMsgLen;
                          decoderChunk.inData =
                              (uint8_t *)malloc(decoderChunk.bytes);
                          memcpy(decoderChunk.inData, codecPayload,
                                 typedMsgLen);
                          decoderChunk.outData = NULL;
                          decoderChunk.type = SNAPCAST_MESSAGE_CODEC_HEADER;

                          flacDecoder = FLAC__stream_decoder_new();
                          if (flacDecoder == NULL) {
                            ESP_LOGE(TAG, "Failed to init flac decoder");
                            return;
                          }

                          FLAC__StreamDecoderInitStatus init_status =
                              FLAC__stream_decoder_init_stream(
                                  flacDecoder, read_callback, NULL, NULL, NULL,
                                  NULL, write_callback, metadata_callback,
                                  error_callback, &scSet);
                          if (init_status !=
                              FLAC__STREAM_DECODER_INIT_STATUS_OK) {
                            ESP_LOGE(TAG, "ERROR: initializing decoder: %s\n",
                                     FLAC__StreamDecoderInitStatusString
                                         [init_status]);

                            return;
                          }

                          FLAC__stream_decoder_process_until_end_of_metadata(
                              flacDecoder);

                          // ESP_LOGI(TAG, "%s: processed codec header",
                          // __func__);
                        } else if (codec == PCM) {
                          uint16_t channels;
                          uint32_t rate;
                          uint16_t bits;

                          memcpy(&channels, codecPayload + 22,
                                 sizeof(channels));
                          memcpy(&rate, codecPayload + 24, sizeof(rate));
                          memcpy(&bits, codecPayload + 34, sizeof(bits));

                          scSet.codec = codec;
                          scSet.bits = bits;
                          scSet.ch = channels;
                          scSet.sr = rate;

                          ESP_LOGI(TAG, "pcm sampleformat: %ld:%d:%d", scSet.sr,
                                   scSet.bits, scSet.ch);
                        } else {
                          ESP_LOGE(TAG,
                                   "codec header decoder "
                                   "shouldn't get here after "
                                   "codec string was detected");

                          return;
                        }

                        free(codecPayload);
                        codecPayload = NULL;

                        if (player_send_snapcast_setting(&scSet) != pdPASS) {
                          ESP_LOGE(TAG,
                                   "Failed to notify sync task. "
                                   "Did you init player?");

                          return;
                        }

                        // ESP_LOGI(TAG, "done codec header msg");

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        received_header = true;
                        esp_timer_stop(timeSyncMessageTimer);
                        if (!esp_timer_is_active(timeSyncMessageTimer)) {
                          esp_timer_start_periodic(timeSyncMessageTimer,
                                                   timeout);
                        }
                      }

                      break;
                    }

                    default: {
                      ESP_LOGE(TAG,
                               "codec header decoder "
                               "shouldn't get here");

                      break;
                    }
                  }

                  break;
                }

                case SNAPCAST_MESSAGE_SERVER_SETTINGS: {
                  switch (internalState) {
                    case 0: {
                      typedMsgLen = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 1: {
                      typedMsgLen |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 2: {
                      typedMsgLen |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 3: {
                      typedMsgLen |= (*start & 0xFF) << 24;

                      // ESP_LOGI(TAG,"server settings string is %lu"
                      //              " long", typedMsgLen);

                      // now get some memory for server settings
                      // string
                      serverSettingsString = malloc(typedMsgLen + 1);
                      if (serverSettingsString == NULL) {
                        ESP_LOGE(TAG,
                                 "couldn't get memory for "
                                 "server settings string");
                      }

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      offset = 0;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 4: {
                      size_t tmpSize =
                          base_message_rx.size - typedMsgCurrentPos;

                      if (len > 0) {
                        if (tmpSize < len) {
                          if (serverSettingsString) {
                            memcpy(&serverSettingsString[offset], start,
                                   tmpSize);
                          }
                          offset += tmpSize;

                          start += tmpSize;
                          // currentPos += tmpSize;  // will be
                          //  incremented by 1
                          //  later so -1 here
                          typedMsgCurrentPos += tmpSize;
                          len -= tmpSize;
                        } else {
                          if (serverSettingsString) {
                            memcpy(&serverSettingsString[offset], start, len);
                          }
                          offset += len;

                          start += len;
                          // currentPos += len;  // will be incremented
                          //  by 1 later so -1
                          //  here
                          typedMsgCurrentPos += len;
                          len = 0;
                        }
                      }

                      if (typedMsgCurrentPos >= base_message_rx.size) {
                        if (serverSettingsString) {
                          // ESP_LOGI(TAG, "done server settings %lu/%lu",
                          //								offset,
                          //								typedMsgLen);

                          // NULL terminate string
                          serverSettingsString[typedMsgLen] = 0;

                          // ESP_LOGI(TAG, "got string: %s",
                          // serverSettingsString);

                          result = server_settings_message_deserialize(
                              &server_settings_message, serverSettingsString);
                          if (result) {
                            ESP_LOGE(TAG,
                                     "Failed to read server "
                                     "settings: %d",
                                     result);
                          } else {
                            // log mute state, buffer, latency
                            ESP_LOGI(TAG, "Buffer length:  %ld",
                                     server_settings_message.buffer_ms);
                            ESP_LOGI(TAG, "Latency:        %ld",
                                     server_settings_message.latency);
                            ESP_LOGI(TAG, "Mute:           %d",
                                     server_settings_message.muted);
                            ESP_LOGI(TAG, "Setting volume: %ld",
                                     server_settings_message.volume);
                          }

                          // Volume setting using ADF HAL
                          // abstraction
                          if (scSet.muted != server_settings_message.muted) {
#if SNAPCAST_USE_SOFT_VOL
                            if (server_settings_message.muted) {
                              dsp_processor_set_volome(0.0);
                            } else {
                              dsp_processor_set_volome(
                                  (double)server_settings_message.volume / 100);
                            }
#endif
                            audio_set_mute(server_settings_message.muted);
                          }

                          if (scSet.volume != server_settings_message.volume) {
#if SNAPCAST_USE_SOFT_VOL
                            if (!server_settings_message.muted) {
                              dsp_processor_set_volome(
                                  (double)server_settings_message.volume / 100);
                            }
#else
                            audio_set_volume(server_settings_message.volume);
#endif
                          }

                          scSet.cDacLat_ms = server_settings_message.latency;
                          scSet.buf_ms = server_settings_message.buffer_ms;
                          scSet.muted = server_settings_message.muted;
                          scSet.volume = server_settings_message.volume;

#if CONFIG_ENABLE_SH1106_DISPLAY
                          // Update display with server volume and mute state
                          display_set_snapcast_volume((uint8_t)scSet.volume);
                          display_set_snapcast_mute(scSet.muted);
#endif

                          if (player_send_snapcast_setting(&scSet) != pdPASS) {
                            ESP_LOGE(TAG,
                                     "Failed to notify sync task. "
                                     "Did you init player?");

                            return;
                          }

                          free(serverSettingsString);
                          serverSettingsString = NULL;
                        }

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        typedMsgCurrentPos = 0;
                      }

                      break;
                    }

                    default: {
                      ESP_LOGE(TAG,
                               "server settings decoder "
                               "shouldn't get here");

                      break;
                    }
                  }

                  break;
                }

                case SNAPCAST_MESSAGE_STREAM_TAGS: {
                  size_t tmpSize = base_message_rx.size - typedMsgCurrentPos;

                  if (tmpSize < len) {
                    start += tmpSize;
                    // currentPos += tmpSize;
                    typedMsgCurrentPos += tmpSize;
                    len -= tmpSize;
                  } else {
                    start += len;
                    // currentPos += len;

                    typedMsgCurrentPos += len;
                    len = 0;
                  }

                  if (typedMsgCurrentPos >= base_message_rx.size) {
                    // ESP_LOGI(TAG,
                    // "done stream tags with length %d %d %d",
                    // base_message_rx.size, currentPos,
                    // tmpSize);

                    typedMsgCurrentPos = 0;
                    // currentPos = 0;

                    state = BASE_MESSAGE_STATE;
                    internalState = 0;
                  }

                  break;
                }

                case SNAPCAST_MESSAGE_TIME: {
                  switch (internalState) {
                    case 0: {
                      time_message_rx.latency.sec = *start;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 1: {
                      time_message_rx.latency.sec |= (int32_t)*start << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 2: {
                      time_message_rx.latency.sec |= (int32_t)*start << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 3: {
                      time_message_rx.latency.sec |= (int32_t)*start << 24;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 4: {
                      time_message_rx.latency.usec = *start;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 5: {
                      time_message_rx.latency.usec |= (int32_t)*start << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 6: {
                      time_message_rx.latency.usec |= (int32_t)*start << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 7: {
                      time_message_rx.latency.usec |= (int32_t)*start << 24;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;
                      if (typedMsgCurrentPos >= base_message_rx.size) {
                        // ESP_LOGI(TAG, "done time message");

                        typedMsgCurrentPos = 0;

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        trx =
                            (int64_t)base_message_rx.received.sec * 1000000LL +
                            (int64_t)base_message_rx.received.usec;
                        ttx = (int64_t)base_message_rx.sent.sec * 1000000LL +
                              (int64_t)base_message_rx.sent.usec;
                        tdif = trx - ttx;
                        trx = (int64_t)time_message_rx.latency.sec * 1000000LL +
                              (int64_t)time_message_rx.latency.usec;
                        tmpDiffToServer = (trx - tdif) / 2;

                        int64_t diff;

                        // clear diffBuffer if last update is
                        // older than a minute
                        diff = now - lastTimeSync;
                        if (diff > 60000000LL) {
                          ESP_LOGW(TAG,
                                   "Last time sync older "
                                   "than a minute. "
                                   "Clearing time buffer");

                          reset_latency_buffer();

                          timeout = FAST_SYNC_LATENCY_BUF;

                          esp_timer_stop(timeSyncMessageTimer);
                          if (received_header == true) {
                            if (!esp_timer_is_active(timeSyncMessageTimer)) {
                              esp_timer_start_periodic(timeSyncMessageTimer,
                                                       timeout);
                            }
                          }
                        }

                        player_latency_insert(tmpDiffToServer);

                        // ESP_LOGI(TAG, "Current latency:%lld:",
                        // tmpDiffToServer);

                        // store current time
                        lastTimeSync = now;

                        if (received_header == true) {
                          if (!esp_timer_is_active(timeSyncMessageTimer)) {
                            esp_timer_start_periodic(timeSyncMessageTimer,
                                                     timeout);
                          }

                          bool is_full = false;
                          latency_buffer_full(&is_full, portMAX_DELAY);
                          if ((is_full == true) &&
                              (timeout < NORMAL_SYNC_LATENCY_BUF)) {
                            timeout = NORMAL_SYNC_LATENCY_BUF;

                            ESP_LOGI(TAG, "latency buffer full");

                            if (esp_timer_is_active(timeSyncMessageTimer)) {
                              esp_timer_stop(timeSyncMessageTimer);
                            }

                            esp_timer_start_periodic(timeSyncMessageTimer,
                                                     timeout);
                          } else if ((is_full == false) &&
                                     (timeout > FAST_SYNC_LATENCY_BUF)) {
                            timeout = FAST_SYNC_LATENCY_BUF;

                            ESP_LOGI(TAG, "latency buffer not full");

                            if (esp_timer_is_active(timeSyncMessageTimer)) {
                              esp_timer_stop(timeSyncMessageTimer);
                            }

                            esp_timer_start_periodic(timeSyncMessageTimer,
                                                     timeout);
                          }
                        }
                      } else {
                        ESP_LOGE(TAG,
                                 "error time message, this "
                                 "shouldn't happen! %d %ld",
                                 typedMsgCurrentPos, base_message_rx.size);

                        typedMsgCurrentPos = 0;

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;
                      }

                      break;
                    }

                    default: {
                      ESP_LOGE(TAG,
                               "time message decoder shouldn't "
                               "get here %d %ld %ld",
                               typedMsgCurrentPos, base_message_rx.size,
                               internalState);

                      break;
                    }
                  }

                  break;
                }

                default: {
                  typedMsgCurrentPos++;
                  start++;
                  // currentPos++;
                  len--;

                  if (typedMsgCurrentPos >= base_message_rx.size) {
                    ESP_LOGI(TAG, "done unknown typed message %d",
                             base_message_rx.type);

                    state = BASE_MESSAGE_STATE;
                    internalState = 0;

                    typedMsgCurrentPos = 0;
                  }

                  break;
                }
              }

              break;
            }

            default: {
              break;
            }
          }

          if (rc1 != ERR_OK) {
            break;
          }
        }
      } while (netbuf_next(firstNetBuf) >= 0);

      netbuf_delete(firstNetBuf);

      if (rc1 != ERR_OK) {
        ESP_LOGE(TAG, "Data error, closing netconn");

        netconn_close(lwipNetconn);

        break;
      }
    }
  }
}

/**
 * Initialize volume control buttons (GPIO 27 & 14) with pull-down configuration
 * Buttons connect to 3.3V for active HIGH logic
 * Supports both Snapcast and Bluetooth A2DP volume control
 */
static void init_volume_buttons(void) {
    gpio_config_t vol_gpio_config = {
        .pin_bit_mask = (1ULL << VOLUME_UP_GPIO) | (1ULL << VOLUME_DOWN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,    // DISABLE pull-ups
        .pull_down_en = GPIO_PULLDOWN_ENABLE, // ENABLE pull-downs (buttons to 3.3V)
        .intr_type = GPIO_INTR_DISABLE        // Polling mode for volume buttons
    };
    
    esp_err_t ret = gpio_config(&vol_gpio_config);
    if (ret != ESP_OK) {
        ESP_LOGE("VOLUME", "Failed to configure volume GPIO pins: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI("VOLUME", "Volume buttons initialized: UP=GPIO%d, DOWN=GPIO%d", VOLUME_UP_GPIO, VOLUME_DOWN_GPIO);
}

#ifdef CONFIG_ENABLE_LED_CONTROLLER
/**
 * Initialize LED effect toggle button GPIO with pull-down configuration
 * Button connects to 3.3V for active HIGH logic
 */
static void init_effect_button(void) {
  gpio_config_t eff_gpio_config = {
    .pin_bit_mask = (1ULL << LED_EFFECT_BUTTON_GPIO),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_ENABLE,
    .intr_type = GPIO_INTR_DISABLE
  };

  esp_err_t ret = gpio_config(&eff_gpio_config);
  if (ret != ESP_OK) {
    ESP_LOGE("VOLUME", "Failed to configure LED effect button GPIO %d: %s", LED_EFFECT_BUTTON_GPIO, esp_err_to_name(ret));
    return;
  }

  ESP_LOGI("VOLUME", "LED effect button initialized on GPIO%d", LED_EFFECT_BUTTON_GPIO);
}

/**
 * Initialize LED controller for sound-reactive effects
 */
static void init_led_controller(void)
{
    // Try to load config from NVS first
    led_config_nvs_t nvs_config;
    esp_err_t ret = led_config_load_from_nvs(&nvs_config);
    
    led_config_t led_config;
    
    if (ret == ESP_OK) {
        // Use NVS configuration
        ESP_LOGI("LED", "Loading LED config from NVS: %d LEDs on GPIO %d", 
                 nvs_config.num_leds, nvs_config.gpio_pin);
      if (!nvs_config.enabled || nvs_config.num_leds == 0) {
        ESP_LOGW("LED", "LED controller disabled by config (enabled=%d, num_leds=%d)",
             nvs_config.enabled, nvs_config.num_leds);
        return;
      }
        led_config.gpio_pin = nvs_config.gpio_pin;
        led_config.num_leds = nvs_config.num_leds;
        led_config.brightness = nvs_config.brightness;
        led_config.effect = nvs_config.effect;
        led_config.speed = nvs_config.speed;
        led_config.sensitivity = nvs_config.sensitivity;
        led_config.color_order = nvs_config.color_order;
        led_config.ignore_volume = nvs_config.ignore_volume;
        led_config.bass_focus = nvs_config.bass_focus;
        led_config.auto_rainbow = nvs_config.auto_rainbow;
        led_config.rainbow_speed = nvs_config.rainbow_speed;
        led_config.auto_rainbow2 = nvs_config.auto_rainbow2;
        led_config.rainbow_speed2 = nvs_config.rainbow_speed2;
        led_config.color1.r = nvs_config.color_r;
        led_config.color1.g = nvs_config.color_g;
        led_config.color1.b = nvs_config.color_b;
        // Load secondary color (falls back to defaults if not present)
        led_config.color2.r = nvs_config.color2_r;
        led_config.color2.g = nvs_config.color2_g;
        led_config.color2.b = nvs_config.color2_b;
    } else {
        // Fall back to Kconfig defaults
        ESP_LOGI("LED", "No NVS config found, using Kconfig defaults");
        led_config.gpio_pin = CONFIG_LED_GPIO_PIN;
        led_config.num_leds = CONFIG_LED_NUM_LEDS;
        led_config.brightness = CONFIG_LED_DEFAULT_BRIGHTNESS;
        led_config.effect = CONFIG_LED_DEFAULT_EFFECT;
        led_config.speed = CONFIG_LED_DEFAULT_SPEED;
        led_config.sensitivity = CONFIG_LED_DEFAULT_SENSITIVITY;
        led_config.color_order = 0;
        led_config.ignore_volume = 0;
        led_config.bass_focus = 128;
        led_config.auto_rainbow = 0;
        led_config.rainbow_speed = 128;
        led_config.auto_rainbow2 = 0;
        led_config.rainbow_speed2 = 128;
        led_config.color1.r = 255;
        led_config.color1.g = 0;
        led_config.color1.b = 0;
        // Kconfig defaults: primary red, secondary blue
        led_config.color2 = (led_color_t){.r = 0, .g = 0, .b = 255};
    }
    
    ret = led_controller_init(&led_config);
    if (ret == ESP_OK) {
        ESP_LOGI("LED", "LED controller initialized: %d LEDs on GPIO %d", 
                 led_config.num_leds, led_config.gpio_pin);
        led_initialized = true;
    } else {
        ESP_LOGE("LED", "Failed to initialize LED controller: %s", 
                 esp_err_to_name(ret));
    }
}
#endif

/**
 * Send JSON-RPC Client.SetVolume request to snapcast server
 * Updates the server with the current volume level for proper synchronization
 */
static void send_volume_update_to_server(int volume_percent) {
    // Get external references
    extern struct netconn *lwipNetconn;
    extern ip_addr_t remote_ip;

    // If we don't have a valid Snapserver connection yet, skip update
    if (lwipNetconn == NULL || remote_ip.u_addr.ip4.addr == 0) {
      ESP_LOGW("SC", "Skipping volume update, no Snapserver connection");
      return;
    }

    ESP_LOGI("SC", "Attempting volume update to server: %d%%", volume_percent);
    
    // Get client MAC address for the client ID
    uint8_t base_mac[6];
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    esp_read_mac(base_mac, ESP_MAC_ETH);
#else
    esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
#endif
    
    char mac_address[18];
    sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", 
            base_mac[0], base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
    
    // Create JSON-RPC Client.SetVolume request
    cJSON *json_request = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    cJSON *volume = cJSON_CreateObject();
    
    cJSON_AddStringToObject(json_request, "id", "8");
    cJSON_AddStringToObject(json_request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(json_request, "method", "Client.SetVolume");
    
    cJSON_AddStringToObject(params, "id", mac_address);
    cJSON_AddBoolToObject(volume, "muted", (volume_percent == 0));
    cJSON_AddNumberToObject(volume, "percent", volume_percent);
    cJSON_AddItemToObject(params, "volume", volume);
    cJSON_AddItemToObject(json_request, "params", params);
    
    char *json_string = cJSON_Print(json_request);
    if (json_string != NULL) {
      char server_url[64];
      snprintf(server_url, sizeof(server_url), "http://" IPSTR ":1780/jsonrpc",
           IP2STR(&remote_ip.u_addr.ip4));

      // Try a few short, synchronous attempts to increase reliability
      // without blocking too long on any single failure.
      const int max_attempts = 3;
      for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        esp_http_client_config_t config = {
          .url = server_url,
          .method = HTTP_METHOD_POST,
          .timeout_ms = 1000,  // LAN should respond fast; keep it short
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
          ESP_LOGW("SC", "Failed to init HTTP client for volume update (attempt %d)", attempt);
          continue;
        }

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, json_string, strlen(json_string));

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
          ESP_LOGI("SC", "Volume update sent successfully on attempt %d", attempt);
          esp_http_client_cleanup(client);
          break;
        } else {
          ESP_LOGW("SC", "HTTP volume update failed (attempt %d/%d): %s",
               attempt, max_attempts, esp_err_to_name(err));
          esp_http_client_cleanup(client);
          if (attempt < max_attempts) {
            vTaskDelay(pdMS_TO_TICKS(100));
          }
        }
      }

      free(json_string);
    }
    cJSON_Delete(json_request);
}

  #ifdef CONFIG_ENABLE_LED_CONTROLLER
  /**
   * LED effect toggle button task
   * Each press toggles between the current effect and OFF
   */
  static void effect_button_task(void *pvParameters) {
    ESP_LOGI("VOLUME", "LED effect button task started");

    // Small delay to ensure LED controller is initialized
    vTaskDelay(pdMS_TO_TICKS(200));

    int last_state = 0;
    uint32_t last_toggle_time = 0;

    led_effect_t last_non_off_effect = LED_EFFECT_VU_METER;
    bool last_effect_valid = false;

    while (1) {
      uint32_t current_time = esp_timer_get_time() / 1000; // ms

      int state = gpio_get_level(LED_EFFECT_BUTTON_GPIO);

      // Rising edge detection with debounce
      if (state == 1 && last_state == 0 && (current_time - last_toggle_time) > VOLUME_BUTTON_DEBOUNCE_MS) {
        last_toggle_time = current_time;

        led_config_t led_cfg;
        if (led_controller_get_config(&led_cfg) == ESP_OK) {
          if (led_cfg.effect == LED_EFFECT_OFF) {
            // Restore last non-off effect if we have one, otherwise use VU Meter
            led_effect_t target = last_effect_valid ? last_non_off_effect : LED_EFFECT_VU_METER;
            ESP_LOGI("VOLUME", "Effect button: restoring effect %d", target);
            led_controller_set_effect(target);
          } else {
            // Remember current effect and turn LEDs off
            last_non_off_effect = led_cfg.effect;
            last_effect_valid = true;
            ESP_LOGI("VOLUME", "Effect button: turning LEDs OFF (was %d)", led_cfg.effect);
            led_controller_set_effect(LED_EFFECT_OFF);
          }
        }
      }

      last_state = state;

      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
  #endif

/**
 * Volume control task - polls GPIO 27 (up) and GPIO 14 (down) buttons
 * Uses pull-down logic: button press connects GPIO HIGH (to 3.3V)
 * Integrates with both Snapcast and Bluetooth audio systems
 * Automatically detects mode and uses appropriate volume control
 */
static void volume_button_task(void *pvParameters) {
    ESP_LOGI("VOLUME", "Volume button task started");
    
    // Audio system already initialized - minimal delay
    vTaskDelay(pdMS_TO_TICKS(100));
    
    uint32_t last_vol_up_time = 0;
    uint32_t last_vol_down_time = 0;
    
    while (1) {
        uint32_t current_time = esp_timer_get_time() / 1000;  // Convert to milliseconds
        
        // Read volume button states (pull-down: HIGH when pressed)
        int vol_up_state = gpio_get_level(VOLUME_UP_GPIO);    
        int vol_down_state = gpio_get_level(VOLUME_DOWN_GPIO); 
        
#if CONFIG_BT_ENABLED
        bool bt_connected = bt_audio_sink_is_connected();
        
        // Only log on Bluetooth connection state changes
        static bool last_bt_state = false;
        static int last_bt_volume = -1;
        if (bt_connected != last_bt_state) {
            ESP_LOGI("VOLUME", "Bluetooth connection: %s", bt_connected ? "CONNECTED" : "DISCONNECTED");
            last_bt_state = bt_connected;
            last_bt_volume = -1; // Reset volume tracking on connection change
        }
        
        // Only log volume changes when connected
        if (bt_connected) {
            int current_bt_volume = bt_audio_get_volume_percent();
            if (current_bt_volume != last_bt_volume) {
                ESP_LOGI("VOLUME", "BT volume: %d%%", current_bt_volume);
                last_bt_volume = current_bt_volume;
            }
        }
#else
        bool bt_connected = false;
#endif
        
        // Volume UP (GPIO 27) - Active HIGH (button connects to 3.3V)
        if (vol_up_state == 1) {
            if (current_time - last_vol_up_time > VOLUME_BUTTON_DEBOUNCE_MS) {
                ESP_LOGI("VOLUME", "Volume UP pressed");
                
                if (bt_connected) {
                    // Bluetooth mode - use A2DP volume control
                    int current_bt_volume = bt_audio_get_volume_percent();
                    int new_volume = current_bt_volume + 10;
                    if (new_volume > 100) new_volume = 100;
                    
#if CONFIG_BT_ENABLED
                    bt_audio_volume_up();
#endif
                } else {
                    // Snapcast mode - use existing snapcast volume control
                    int current_volume = scSet.volume;  // Use server's current volume
                    int new_volume = current_volume + 6;
                    if (new_volume > 100) new_volume = 100;
                    
                    ESP_LOGI("SC", "Snapcast Volume UP: %d%% -> %d%%", current_volume, new_volume);
                    
                    audio_set_volume(new_volume);
                    send_volume_update_to_server(new_volume);
                }
                
                last_vol_up_time = current_time;
            }
        }
        
        // Volume DOWN (GPIO 14) - Active HIGH (button connects to 3.3V)
        if (vol_down_state == 1) {
            if (current_time - last_vol_down_time > VOLUME_BUTTON_DEBOUNCE_MS) {
                ESP_LOGI("VOLUME", "Volume DOWN pressed");
                
                if (bt_connected) {
                    // Bluetooth mode - use A2DP volume control
                    int current_bt_volume = bt_audio_get_volume_percent();
                    int new_volume = current_bt_volume - 10;
                    if (new_volume < 0) new_volume = 0;
                    
#if CONFIG_BT_ENABLED
                    bt_audio_volume_down();
#endif
                } else {
                    // Snapcast mode - use existing snapcast volume control
                    int current_volume = scSet.volume;  // Use server's current volume
                    int new_volume = current_volume - 6;
                    if (new_volume < 0) new_volume = 0;
                    
                    ESP_LOGI("SC", "Snapcast Volume DOWN: %d%% -> %d%%", current_volume, new_volume);
                    
                    audio_set_volume(new_volume);
                    send_volume_update_to_server(new_volume);
                }
                
                last_vol_down_time = current_time;
            }
        }
        
        // Poll every 50ms for responsive button handling
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

#if CONFIG_ENABLE_SH1106_DISPLAY
/**
 * WiFi Signal Monitoring Functions
 */
static void update_wifi_signal_display(void) {
    // Update the display with current WiFi signal strength
    display_set_wifi_signal(current_rssi, true);  // Assume connected if we're running
}

static void wifi_signal_monitor_task(void *pvParameters) {
    wifi_ap_record_t ap_record;
    
    ESP_LOGI("SC", "WiFi signal monitor task started");
    
    // Wait for WiFi to be connected before starting monitoring
    while (1) {
        wifi_ap_record_t ap_info;
        esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
        
        if (ret == ESP_OK) {
            ESP_LOGI("SC", "WiFi connected, starting signal monitoring");
            break;
        }
        
        // WiFi not connected yet, wait and try again
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    
    while (1) {
        esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_record);
        
        if (ret == ESP_OK) {
            current_rssi = ap_record.rssi;
            
            // Log significant RSSI changes
            static int32_t last_logged_rssi = 0;
            if (abs(current_rssi - last_logged_rssi) >= 5) {
                const char* quality = "Unknown";
                if (current_rssi >= -50) quality = "Excellent";
                else if (current_rssi >= -60) quality = "Good";
                else if (current_rssi >= -70) quality = "Fair";
                else if (current_rssi >= -80) quality = "Poor";
                else quality = "Very Poor";
                
                ESP_LOGI("SC", "WiFi RSSI: %"PRId32" dBm (%s)", current_rssi, quality);
                last_logged_rssi = current_rssi;
            }
            
            // Update display with current RSSI
            update_wifi_signal_display();
            
        } else {
            // WiFi disconnected
            current_rssi = -100;
            ESP_LOGW("SC", "WiFi disconnected - RSSI unavailable");
            display_set_wifi_signal(-100, false);  // Show disconnected state
        }
        
        // Update every 3 seconds (balance between responsiveness and overhead)
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
#endif // CONFIG_ENABLE_SH1106_DISPLAY

/**
 *
 */
void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Load system configuration (Snapclient name, volume buttons, gain boost)
  system_config_set_defaults(&g_system_config);
  esp_err_t sys_cfg_ret = system_config_load_from_nvs(&g_system_config);
  if (sys_cfg_ret == ESP_OK) {
    ESP_LOGI(TAG,
             "System config loaded: name='%s', vol_buttons=%s, up=%d, down=%d, gain=%.2f",
             g_system_config.snapclient_name,
             g_system_config.volume_buttons_enabled ? "on" : "off",
             g_system_config.volume_up_pin,
             g_system_config.volume_down_pin,
             (double)g_system_config.snapcast_gain_boost);
        ESP_LOGI(TAG, "Loaded display column offset from NVS: %d",
             g_system_config.sh1106_column_offset);
  } else {
    ESP_LOGI(TAG,
             "Using default system config: name='%s', vol_buttons=%s, up=%d, down=%d, gain=%.2f",
             g_system_config.snapclient_name,
             g_system_config.volume_buttons_enabled ? "on" : "off",
             g_system_config.volume_up_pin,
             g_system_config.volume_down_pin,
             (double)g_system_config.snapcast_gain_boost);
        ESP_LOGI(TAG, "Using default display column offset: %d",
             g_system_config.sh1106_column_offset);
  }

  // Default: show info-level logs globally
  esp_log_level_set("*", ESP_LOG_INFO);

  // Quiet down chatty modules to save CPU/flash and avoid log spam
  esp_log_level_set("BT_AUDIO_SINK", ESP_LOG_WARN);  // only warnings/errors from Bluetooth sink
  esp_log_level_set("LED_CTRL", ESP_LOG_WARN);       // suppress per-frame/effect info logs
  esp_log_level_set("HTTP", ESP_LOG_WARN);           // reduce HTTP/UI chatter
  esp_log_level_set("SC", ESP_LOG_WARN);             // Snapcast core (buffer/latency/volume spam)
  esp_log_level_set("SYS_CONFIG", ESP_LOG_WARN);     // system config load/save spam
  esp_log_level_set("dspProc", ESP_LOG_WARN);        // per-volume DSP logs
  esp_log_level_set("PLAYER", ESP_LOG_WARN);         // player status logs on every change
  esp_log_level_set("VOLUME", ESP_LOG_WARN);         // volume button/BT volume logs

  // if enabled these cause a timer srv stack overflow
  esp_log_level_set("HEADPHONE", ESP_LOG_NONE);
  esp_log_level_set("gpio", ESP_LOG_WARN);
  esp_log_level_set("uart", ESP_LOG_WARN);
  // esp_log_level_set("i2s_std", ESP_LOG_DEBUG);
  // esp_log_level_set("i2s_common", ESP_LOG_DEBUG);
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("wifi_init", ESP_LOG_WARN);
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("wifi_init", ESP_LOG_WARN);

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  // clang-format off
  // nINT/REFCLKO Function Select Configuration Strap
  //  • When nINTSEL is floated or pulled to
  //    VDD2A, nINT is selected for operation on the
  //    nINT/REFCLKO pin (default).
  //  • When nINTSEL is pulled low to VSS, REF-
  //    CLKO is selected for operation on the nINT/
  //    REFCLKO pin.
  //
  // LAN8720 doesn't stop REFCLK while in reset, so we leave the
  // strap floated. It is connected to IO0 on ESP32 so we get nINT
  // function with a HIGH pin value, which is also perfect during boot.
  // Before initializing LAN8720 (which resets the PHY) we pull the
  // strap low and this results in REFCLK enabled which is needed
  // for MAC unit.
  //
  // clang-format on
  gpio_config_t cfg = {.pin_bit_mask = BIT64(GPIO_NUM_5),
                       .mode = GPIO_MODE_DEF_INPUT,
                       .pull_up_en = GPIO_PULLUP_DISABLE,
                       .pull_down_en = GPIO_PULLDOWN_ENABLE,
                       .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&cfg);
#endif

  board_i2s_pin_t pin_config0;
  get_i2s_pins(I2S_NUM_0, &pin_config0);

#if CONFIG_AUDIO_BOARD_CUSTOM && CONFIG_DAC_ADAU1961
  // some codecs need i2s mclk for initialization

  i2s_chan_handle_t tx_chan;

  i2s_chan_config_t tx_chan_cfg = {
      .id = I2S_NUM_0,
      .role = I2S_ROLE_MASTER,
      .dma_desc_num = 2,
      .dma_frame_num = 128,
      .auto_clear = true,
  };
  ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL));

  i2s_std_clk_config_t i2s_clkcfg = {
      .sample_rate_hz = 44100,
      .clk_src = I2S_CLK_SRC_APLL,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  };
  i2s_std_config_t tx_std_cfg = {
      .clk_cfg = i2s_clkcfg,
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = pin_config0
                          .mck_io_num,  // some codecs may require mclk signal,
                                        // this example doesn't need it
              .bclk = pin_config0.bck_io_num,
              .ws = pin_config0.ws_io_num,
              .dout = pin_config0.data_out_num,
              .din = pin_config0.data_in_num,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg));
  i2s_channel_enable(tx_chan);
#endif

  ESP_LOGI(TAG, "Start codec chip");
  audio_board_handle_t board_handle = audio_board_init();
  if (board_handle) {
    ESP_LOGI(TAG, "Audio board_init done");
  } else {
    ESP_LOGE(TAG,
             "Audio board couldn't be initialized. Check menuconfig if project "
             "is configured right or check your wiring!");

    vTaskDelay(portMAX_DELAY);
  }

  audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE,
                       AUDIO_HAL_CTRL_START);
  audio_hal_set_mute(board_handle->audio_hal,
                     true);  // ensure no noise is sent after firmware crash

#if CONFIG_ENABLE_SH1106_DISPLAY
  ESP_LOGI("DUAL_OTA", "Initializing SH1106 OLED display");
  if (!g_system_config.sh1106_enabled) {
    ESP_LOGI("DUAL_OTA", "SH1106 display disabled by system config");
  } else {
    esp_err_t display_ret = display_init_with_i2c(
        g_system_config.sh1106_sda_gpio,
        g_system_config.sh1106_scl_gpio,
        g_system_config.sh1106_i2c_freq_hz);
    if (display_ret == ESP_OK) {
      ESP_LOGI("DUAL_OTA", "SH1106 display initialized successfully");
    
    // Clear display to remove any previous state
    ESP_LOGI("DUAL_OTA", "Clearing display and starting display task");
    display_clear();
    
    // Start the display task (crucial for screen updates!)
    display_task_start();
    
    // Set the local device name on the display from system config
    display_set_device_name(g_system_config.snapclient_name);

    // Apply runtime column offset from system config (0 for 0.96" panels,
    // 2 for 1.28" SH1106 by convention). This must be set after init,
    // before regular updates start.
    extern void display_set_column_offset(int offset);
    display_set_column_offset(g_system_config.sh1106_column_offset);
    ESP_LOGI("DISPLAY", "Applied runtime column offset: %d",
         g_system_config.sh1106_column_offset);
    
    // Small delay to let display task initialize
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Set initial metadata for snapclient mode
    display_set_song_metadata("Snapclient", "Starting up...", "", "");
    
    // Set initial audio status with all bars at baseline level
    uint8_t initial_eq_levels[16] = {20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20};
    display_set_audio_levels(initial_eq_levels, 50, false);
    display_set_snapcast_volume(50); // Set initial snapcast volume
    display_set_snapcast_mute(false); // Set initial snapcast mute state
    display_set_connection_status(false);
    
    // Force an immediate display update
    display_update();
      ESP_LOGI("DUAL_OTA", "Snapclient display content set and updated");
    } else {
      ESP_LOGW("DUAL_OTA", "Failed to initialize SH1106 display: %s", esp_err_to_name(display_ret));
    }
  }
#endif

#if CONFIG_AUDIO_BOARD_CUSTOM && CONFIG_DAC_ADAU1961
  if (tx_chan) {
    i2s_channel_disable(tx_chan);
    i2s_del_channel(tx_chan);
    tx_chan = NULL;
  }
#endif

  //  ESP_LOGI(TAG, "init player");
  i2s_std_gpio_config_t i2s_pin_config0 =
  {
      .mclk = pin_config0.mck_io_num,
      .bclk = pin_config0.bck_io_num,
      .ws = pin_config0.ws_io_num,
      .dout = pin_config0.data_out_num,
      .din = pin_config0.data_in_num,
      .invert_flags =
          {
#if CONFIG_INVERT_MCLK_LEVEL
              .mclk_inv = true,

#else
              .mclk_inv = false,
#endif

#if CONFIG_INVERT_BCLK_LEVEL
              .bclk_inv = true,
#else
              .bclk_inv = false,
#endif

#if CONFIG_INVERT_WORD_SELECT_LEVEL
              .ws_inv = true,
#else
              .ws_inv = false,
#endif
          },
  };

  QueueHandle_t audioQHdl = xQueueCreate(1, sizeof(audioDACdata_t));

  init_snapcast(audioQHdl);
  init_player(i2s_pin_config0, I2S_NUM_0);

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  eth_init();
  // pass "WIFI_STA_DEF", "WIFI_AP_DEF", "ETH_DEF"
  init_http_server_task("ETH_DEF");
#else
  // Enable and setup WIFI in station mode and connect to Access point setup in
  // menu config or set up provisioning mode settable in menuconfig
  wifi_init();
  ESP_LOGI(TAG, "Connected to AP");
  // http server for control operations and user interface
  // pass "WIFI_STA_DEF", "WIFI_AP_DEF", "ETH_DEF"
  init_http_server_task("WIFI_STA_DEF");
#endif

#ifdef CONFIG_ENABLE_LED_CONTROLLER
  init_led_controller();
#endif

  // Enable websocket server
  //  ESP_LOGI(TAG, "Setup ws server");
  //  websocket_if_start();

#if CONFIG_BT_ENABLED
  bt_audio_init();
#endif

  net_mdns_register("snapclient");
#ifdef CONFIG_SNAPCLIENT_SNTP_ENABLE
  set_time_from_sntp();
#endif

#if CONFIG_USE_DSP_PROCESSOR
  dsp_processor_init();
#endif

  xTaskCreatePinnedToCore(&ota_server_task, "ota", 14 * 256, NULL,
                          OTA_TASK_PRIORITY, &t_ota_task, OTA_TASK_CORE_ID);

  xTaskCreatePinnedToCore(&http_get_task, "http", 15 * 1024, NULL,
                          HTTP_TASK_PRIORITY, &t_http_get_task,
                          HTTP_TASK_CORE_ID);

#if CONFIG_ENABLE_SH1106_DISPLAY
  // Start WiFi signal monitoring task
  ESP_LOGI("DUAL_OTA", "Starting WiFi signal monitoring task");
  xTaskCreate(wifi_signal_monitor_task, "wifi_signal", 3072, NULL, 2, &wifi_monitor_task_handle);
  ESP_LOGI("DUAL_OTA", "WiFi signal monitoring started");
#endif

  // Initialize volume control from system configuration if enabled
  if (g_system_config.volume_buttons_enabled) {
    ESP_LOGI("DUAL_OTA",
             "Audio system ready - initializing volume control (UP=GPIO%d, DOWN=GPIO%d)",
             g_system_config.volume_up_pin,
             g_system_config.volume_down_pin);
    init_volume_buttons();
    xTaskCreate(volume_button_task, "volume_buttons", 4096, NULL, 1, NULL);
    ESP_LOGI("DUAL_OTA", "Volume control initialized");
  } else {
    ESP_LOGI("DUAL_OTA", "Volume buttons disabled in system configuration");
  }

#if CONFIG_ENABLE_LED_CONTROLLER
  // Initialize LED effect toggle button if configured
  if (g_system_config.effect_button_enabled) {
    ESP_LOGI("DUAL_OTA", "Initializing LED effect button on GPIO%d", g_system_config.effect_button_pin);
    init_effect_button();
    xTaskCreate(effect_button_task, "effect_button", 2048, NULL, 1, NULL);
  } else {
    ESP_LOGI("DUAL_OTA", "LED effect button disabled in system configuration");
  }
#endif

  // Start AP-mode trigger task using GPIO0 (BOOT button on most boards)
  xTaskCreate(ap_mode_button_task, "ap_button", 2048, NULL, 2, NULL);

  //  while (1) {
  //    // audio_event_iface_msg_t msg;
  //    vTaskDelay(portMAX_DELAY);  //(pdMS_TO_TICKS(5000));
  //
  //    // ma120_read_error(0x20);
  //
  //    esp_err_t ret = 0;  // audio_event_iface_listen(evt, &msg,
  //    portMAX_DELAY); if (ret != ESP_OK) {
  //      ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
  //      continue;
  //    }
  //  }

  audioDACdata_t dac_data;
  audioDACdata_t dac_data_old = {
    .mute = true,
    .volume = 100,
  };

  while(1) {
    if (xQueueReceive(audioQHdl, &dac_data, portMAX_DELAY) == pdTRUE) {
      if (dac_data.mute != dac_data_old.mute){
        audio_hal_set_mute(board_handle->audio_hal, dac_data.mute);
      }
      if (dac_data.volume != dac_data_old.volume){
        audio_hal_set_volume(board_handle->audio_hal, dac_data.volume);
      }
      dac_data_old = dac_data;
    }
  }
}

static void ap_mode_button_task(void *pvParameters) {
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << AP_MODE_BUTTON_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_ENABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io);

  const TickType_t poll = pdMS_TO_TICKS(50);
  uint32_t pressed_ms = 0;
  bool ap_started_logged = false;

  ESP_LOGI("AP_BUTTON", "AP button configured on GPIO%d (active-high)", AP_MODE_BUTTON_GPIO);


  while (1) {
    int level = gpio_get_level(AP_MODE_BUTTON_GPIO);
    bool pressed = (level == 1); // active-high: HIGH means button pressed

    if (pressed) {
      pressed_ms += 50;
      if (pressed_ms >= AP_MODE_HOLD_MS) {
        ESP_LOGI("AP_BUTTON", "GPIO0 held for %d ms, starting recovery AP mode", AP_MODE_HOLD_MS);
        wifi_start_ap_mode();
        if (!ap_started_logged) {
          ESP_LOGI("AP_BUTTON", "Recovery AP should now be available");
          ap_started_logged = true;
        }
        // After triggering once, keep task running but avoid repeated logs
        pressed_ms = 0;
      }
    } else {
      pressed_ms = 0;
    }

    vTaskDelay(poll);
  }
}