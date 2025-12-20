#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

extern QueueHandle_t bluetooth_pcm_queue;

void bt_audio_task_init();
void bt_audio_task(void *pvParameters);

#ifdef __cplusplus
}
#endif
