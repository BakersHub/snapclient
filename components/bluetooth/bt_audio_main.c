#include "bt_audio_main.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "player.h"
#include "bt_audio_sink.h"
#include "bt_audio_task.h"

#define TAG "BT_AUDIO_MAIN"

void bt_audio_init() {
    ESP_LOGI(TAG, "Initializing Bluetooth audio");
    // Reduced queue size from 6 to 3 to reduce memory pressure
    bluetooth_pcm_queue = xQueueCreate(3, sizeof(pcm_chunk_message_t *));
    bt_audio_task_init();
    bt_audio_sink_init();
}
