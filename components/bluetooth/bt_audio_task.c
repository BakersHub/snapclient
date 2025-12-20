#include "bt_audio_task.h"

#include "esp_log.h"

#include "player.h"

#define TAG "BT_AUDIO_TASK"

QueueHandle_t bluetooth_pcm_queue;

extern void audio_set_mute(bool mute);

void bt_audio_task_init() {
    xTaskCreatePinnedToCore(bt_audio_task, "bt_audio_task", 4096, NULL, 5, NULL, 1);
}

/*void print_hex(const void *data, size_t length) {
    const unsigned char *bytes = (const unsigned char *)data;
    for (size_t i = 0; i < length; i++) {
        printf("%02X ", bytes[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (length % 16 != 0) printf("\n");
}*/

void bt_audio_task(void *pvParameters) {
    pcm_chunk_message_t *chunk = NULL;
    char* p_payload = NULL;
    size_t size = 0, written = 0;
    pcm_chunk_fragment_t *fragment = NULL;

    while (1) {
        if (xQueueReceive(bluetooth_pcm_queue, &chunk, portMAX_DELAY) == pdTRUE) {
            fragment = chunk->fragment;
            while (fragment) {
                p_payload = fragment->payload;
                size = fragment->size;
                while (size > 0) {
                    //print_hex(p_payload, size);
                    if (overridden_player_write(p_payload, size, &written, portMAX_DELAY) != ESP_OK) {
                        ESP_LOGE(TAG, "I2S write error");
                        break;
                    }
                    size -= written;
                    p_payload += written;
                }
                fragment = fragment->nextFragment;
            }
            free_pcm_chunk(chunk);
        }
    }
}
