

#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <math.h>

#include "freertos/FreeRTOS.h"

#if CONFIG_USE_DSP_PROCESSOR
#include "dsps_biquad.h"
#include "dsps_biquad_gen.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "nvs_flash.h"

#include "dsp_processor.h"
#include "system_config.h"

#ifndef CONFIG_SNAPCAST_GAIN_BOOST
#define SNAPCAST_GAIN_BOOST_DEFAULT 0.1f
#else
#define SNAPCAST_GAIN_BOOST_DEFAULT atof(CONFIG_SNAPCAST_GAIN_BOOST)
#endif

#ifdef CONFIG_USE_BIQUAD_ASM
#define BIQUAD dsps_biquad_f32_ae32
#else
#define BIQUAD dsps_biquad_f32
#endif

static const char *TAG = "dspProc";

#define DSP_PROCESSOR_LEN 16

#define NVS_NAMESPACE_DSP "dspcfg"

static QueueHandle_t filterUpdateQHdl = NULL;

static filterParams_t filterParams;

static ptype_t *filter = NULL;

static double dynamic_vol = 1.0;
static double dynamic_vol_base = 1.0; // unboosted volume in 0..1 for mapping curve

static bool init = false;

static float *sbuffer0 = NULL;
static float *sbufout0 = NULL;

// Dynamic bass mapping state
static bool dynamic_bass_enabled = false;
static float bass_low_gain = 0.0f;
static float bass_high_gain = 0.0f;
static float last_dynamic_bass_gain = 0.0f; // Track last applied gain to avoid unnecessary filter updates

// Dynamic treble mapping state
static bool dynamic_treble_enabled = false;
static float treble_low_gain = 0.0f;
static float treble_high_gain = 0.0f;
static float last_dynamic_treble_gain = 0.0f;

static float get_runtime_gain_boost(void) {
  system_config_t cfg;
  esp_err_t err = system_config_load_from_nvs(&cfg);
  if (err == ESP_OK && cfg.snapcast_gain_boost > 0.0f &&
      cfg.snapcast_gain_boost < 10.0f) {
    return cfg.snapcast_gain_boost;
  }

  return SNAPCAST_GAIN_BOOST_DEFAULT;
}

static void dsp_load_filter_params_from_nvs(void) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE_DSP, NVS_READONLY, &nvs);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "No DSP config in NVS, using defaults");
    return;
  } else if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error opening DSP NVS namespace: %s", esp_err_to_name(err));
    return;
  }

  uint8_t flow;
  if (nvs_get_u8(nvs, "dsp_flow", &flow) == ESP_OK && flow <= dspfEQBassTreble) {
    filterParams.dspFlow = (dspFlows_t)flow;
  }

  size_t len = sizeof(float);
  if (nvs_get_blob(nvs, "fc1", &filterParams.fc_1, &len) != ESP_OK) {
    // keep default
  }
  len = sizeof(float);
  if (nvs_get_blob(nvs, "g1", &filterParams.gain_1, &len) != ESP_OK) {
    // keep default
  }
  len = sizeof(float);
  if (nvs_get_blob(nvs, "fc2", &filterParams.fc_2, &len) != ESP_OK) {
    // keep default
  }
  len = sizeof(float);
  if (nvs_get_blob(nvs, "g2", &filterParams.gain_2, &len) != ESP_OK) {
    // keep default
  }
  len = sizeof(float);
  if (nvs_get_blob(nvs, "fc3", &filterParams.fc_3, &len) != ESP_OK) {
    // keep default
  }
  len = sizeof(float);
  if (nvs_get_blob(nvs, "g3", &filterParams.gain_3, &len) != ESP_OK) {
    // keep default
  }

  nvs_close(nvs);

  ESP_LOGI(TAG,
           "Loaded DSP config from NVS: flow=%d, fc1=%.2f, g1=%.2f, fc3=%.2f, g3=%.2f",
           (int)filterParams.dspFlow, filterParams.fc_1, filterParams.gain_1,
           filterParams.fc_3, filterParams.gain_3);
}

static void dsp_save_filter_params_to_nvs(const filterParams_t *params) {
  if (!params) return;

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE_DSP, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error opening DSP NVS namespace for write: %s",
             esp_err_to_name(err));
    return;
  }

  uint8_t flow = (uint8_t)params->dspFlow;
  err = nvs_set_u8(nvs, "dsp_flow", flow);
  if (err != ESP_OK) goto out;

  err = nvs_set_blob(nvs, "fc1", &params->fc_1, sizeof(float));
  if (err != ESP_OK) goto out;

  err = nvs_set_blob(nvs, "g1", &params->gain_1, sizeof(float));
  if (err != ESP_OK) goto out;

  err = nvs_set_blob(nvs, "fc2", &params->fc_2, sizeof(float));
  if (err != ESP_OK) goto out;

  err = nvs_set_blob(nvs, "g2", &params->gain_2, sizeof(float));
  if (err != ESP_OK) goto out;

  err = nvs_set_blob(nvs, "fc3", &params->fc_3, sizeof(float));
  if (err != ESP_OK) goto out;

  err = nvs_set_blob(nvs, "g3", &params->gain_3, sizeof(float));
  if (err != ESP_OK) goto out;

  err = nvs_commit(nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to commit DSP config to NVS: %s",
             esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG,
             "Saved DSP config to NVS: flow=%d, fc1=%.2f, g1=%.2f, fc3=%.2f, g3=%.2f",
             (int)params->dspFlow, params->fc_1, params->gain_1,
             params->fc_3, params->gain_3);
  }

out:
  nvs_close(nvs);
}

#if CONFIG_USE_DSP_PROCESSOR
#if CONFIG_SNAPCLIENT_DSP_FLOW_STEREO
dspFlows_t dspFlowInit = dspfStereo;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BASSBOOST
dspFlows_t dspFlowInit = dspfBassBoost;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BIAMP
dspFlows_t dspFlowInit = dspfBiamp;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BASS_TREBLE_EQ
dspFlows_t dspFlowInit = dspfEQBassTreble;
#endif
#endif

/**
 *
 */
void dsp_processor_init(void) {
  init = false;

  if (filterUpdateQHdl) {
    vQueueDelete(filterUpdateQHdl);
    filterUpdateQHdl = NULL;
  }

  // have a max queue length of 1 here because we use xQueueOverwrite
  // to write to the queue
  filterUpdateQHdl = xQueueCreate(1, sizeof(filterParams_t));
  if (filterUpdateQHdl == NULL) {
    ESP_LOGE(TAG, "%s: Failed to create filter update queue", __func__);
    return;
  }

  filterParams.dspFlow = dspFlowInit;

  switch (filterParams.dspFlow) {
    case dspfEQBassTreble: {
      filterParams.fc_1 = 300.0;
      filterParams.gain_1 = 0.0;
      filterParams.fc_2 = 1000.0;
      filterParams.gain_2 = 0.0;
      filterParams.fc_3 = 4000.0;
      filterParams.gain_3 = 0.0;

      break;
    }

    case dspfStereo: {
      break;
    }

    case dspfBassBoost: {
      filterParams.fc_1 = 300.0;
      filterParams.gain_1 = 6.0;
      break;
    }

    case dspfBiamp: {
      filterParams.fc_1 = 300.0;
      filterParams.gain_1 = 0;
      filterParams.fc_3 = 100.0;
      filterParams.gain_3 = 0.0;
      break;
    }

    case dspf2DOT1: {  // Process audio L + R LOW PASS FILTER
      ESP_LOGW(TAG, "dspf2DOT1, not implemented yet, using stereo instead");
    } break;

    case dspfFunkyHonda: {  // Process audio L + R LOW PASS FILTER
      ESP_LOGW(TAG,
               "dspfFunkyHonda, not implemented yet, using stereo instead");
      break;
    }

    default: { break; }
  }

  // Override defaults from NVS if available
  dsp_load_filter_params_from_nvs();

  // Load bass mapping settings from system config
  system_config_t sys_cfg;
  if (system_config_load_from_nvs(&sys_cfg) == ESP_OK) {
    dynamic_bass_enabled = sys_cfg.bass_mapping_enabled;
    bass_low_gain = sys_cfg.bass_mapping_low_gain;
    bass_high_gain = sys_cfg.bass_mapping_high_gain;
    // Initialize last gain for change detection
    last_dynamic_bass_gain = bass_low_gain + (bass_high_gain - bass_low_gain) * dynamic_vol_base;
    ESP_LOGI(TAG, "Loaded bass mapping: enabled=%d, low_gain=%.1f, high_gain=%.1f, initial_dynamic_gain=%.1f",
             dynamic_bass_enabled, bass_low_gain, bass_high_gain, last_dynamic_bass_gain);

    // Load treble mapping settings
    dynamic_treble_enabled = sys_cfg.treble_mapping_enabled;
    treble_low_gain = sys_cfg.treble_mapping_low_gain;
    treble_high_gain = sys_cfg.treble_mapping_high_gain;
    last_dynamic_treble_gain = treble_low_gain + (treble_high_gain - treble_low_gain) * dynamic_vol_base;
    ESP_LOGI(TAG, "Loaded treble mapping: enabled=%d, low_gain=%.1f, high_gain=%.1f, initial_dynamic_gain=%.1f",
             dynamic_treble_enabled, treble_low_gain, treble_high_gain, last_dynamic_treble_gain);
  }

  ESP_LOGI(TAG, "%s: init done", __func__);
}

/**
 * free previously allocated memories
 */
void dsp_processor_uninit(void) {
  if (sbuffer0) {
    free(sbuffer0);
    sbuffer0 = NULL;
  }

  if (sbufout0) {
    free(sbufout0);
    sbufout0 = NULL;
  }

  if (filter) {
    free(filter);
    filter = NULL;
  }

  if (filterUpdateQHdl) {
    vQueueDelete(filterUpdateQHdl);
    filterUpdateQHdl = NULL;
  }

  init = false;

  ESP_LOGI(TAG, "%s: uninit done", __func__);
}

/**
 *
 */
esp_err_t dsp_processor_update_filter_params(filterParams_t *params) {
  if (filterUpdateQHdl) {
    if (xQueueOverwrite(filterUpdateQHdl, params) == pdTRUE) {
      return ESP_OK;
    }
  }

  return ESP_FAIL;
}

/**
 *
 */
static int32_t dsp_processor_gen_filter(ptype_t *filter, uint32_t cnt) {
  if ((filter == NULL) && (cnt > 0)) {
    return ESP_FAIL;
  }

  for (int n = 0; n < cnt; n++) {
    switch (filter[n].filtertype) {
      case HIGHSHELF:
        dsps_biquad_gen_highShelf_f32(filter[n].coeffs, filter[n].freq,
                                      filter[n].gain, filter[n].q);
        break;

      case LOWSHELF:
        dsps_biquad_gen_lowShelf_f32(filter[n].coeffs, filter[n].freq,
                                     filter[n].gain, filter[n].q);
        break;

      case LPF:
        dsps_biquad_gen_lpf_f32(filter[n].coeffs, filter[n].freq, filter[n].q);
        break;

      case HPF:
        dsps_biquad_gen_hpf_f32(filter[n].coeffs, filter[n].freq, filter[n].q);
        break;

      default:
        break;
    }
    //    for (uint8_t i = 0; i <= 4; i++) {
    //      printf("%.6f ", filter[n].coeffs[i]);
    //    }
    //    printf("\n");
  }

  return ESP_OK;
}

/**
 *
 */
int dsp_processor_worker(char *audio, size_t chunk_size, uint32_t samplerate) {
  int16_t len = chunk_size / 4;
  int16_t valint;
  uint16_t i;
  // volatile needed to ensure 32 bit access
  volatile uint32_t *audio_tmp = (volatile uint32_t *)audio;
  dspFlows_t dspFlow;

  // check if we need to update filters
  if (xQueueReceive(filterUpdateQHdl, &filterParams, pdMS_TO_TICKS(0)) ==
      pdTRUE) {
    init = false;

    // Store updated filter parameters in NVS so they
    // persist across restarts.
    dsp_save_filter_params_to_nvs(&filterParams);
  }

  dspFlow = filterParams.dspFlow;

  if (init == false) {
    uint32_t cnt = 0;

    if (filter) {
      free(filter);
      filter = NULL;
    }

    switch (dspFlow) {
      case dspfEQBassTreble: {
        cnt = 4;  // 2 filters per channel (bass, treble)

        filter =
            (ptype_t *)heap_caps_malloc(sizeof(ptype_t) * cnt, MALLOC_CAP_8BIT);
        if (filter) {
          // EQ control of two frequency ranges: bass, treble
          float bass_fc = filterParams.fc_1 / samplerate;
          float bass_gain = filterParams.gain_1; // base bass gain
          if (dynamic_bass_enabled) {
            float bass_mapping_gain = bass_low_gain + (bass_high_gain - bass_low_gain) * dynamic_vol_base;
            bass_gain += bass_mapping_gain;
          }

          float mids_gain = filterParams.gain_2; // base mids gain (static only, not dynamic)

          float treble_fc = filterParams.fc_3 / samplerate;
          float treble_gain = filterParams.gain_3; // base treble gain
          if (dynamic_treble_enabled) {
            float treble_mapping_gain = treble_low_gain + (treble_high_gain - treble_low_gain) * dynamic_vol_base;
            treble_gain += treble_mapping_gain;
          }

          // filters for CH 0: bass, treble (mids is static only, not DSP processed)
          filter[0] = (ptype_t){LOWSHELF,  bass_fc,   bass_gain,      0.707, NULL, NULL, {0,0,0,0,0}, {0,0}};
          filter[1] = (ptype_t){HIGHSHELF, treble_fc, treble_gain,    0.707, NULL, NULL, {0,0,0,0,0}, {0,0}};
          // filters for CH 1: bass, treble
          filter[2] = (ptype_t){LOWSHELF,  bass_fc,   bass_gain,      0.707, NULL, NULL, {0,0,0,0,0}, {0,0}};
          filter[3] = (ptype_t){HIGHSHELF, treble_fc, treble_gain,    0.707, NULL, NULL, {0,0,0,0,0}, {0,0}};

          ESP_LOGI(TAG, "EQBassTreble: bass gain=%.2f dB, mids gain=%.2f dB (static), treble gain=%.2f dB",
                   bass_gain, mids_gain, treble_gain);
        } else {
          ESP_LOGE(TAG, "failed to get memory for filter");
        }

        break;
      }

      case dspfStereo: {
        cnt = 0;
        break;
      }

      case dspfBassBoost: {
        cnt = 2;

        filter =
            (ptype_t *)heap_caps_malloc(sizeof(ptype_t) * cnt, MALLOC_CAP_8BIT);
        if (filter) {
          float bass_fc = filterParams.fc_1 / samplerate;
          float bass_gain = 6.0;

          filter[0] = (ptype_t){LOWSHELF, bass_fc, bass_gain,       0.707,
                                NULL,     NULL,    {0, 0, 0, 0, 0}, {0, 0}};
          filter[1] = (ptype_t){LOWSHELF, bass_fc, bass_gain,       0.707,
                                NULL,     NULL,    {0, 0, 0, 0, 0}, {0, 0}};

          ESP_LOGI(TAG, "got new setting for dspfBassBoost");
        } else {
          ESP_LOGE(TAG, "failed to get memory for filter");
        }

        break;
      }

      case dspfBiamp: {
        cnt = 4;

        filter =
            (ptype_t *)heap_caps_malloc(sizeof(ptype_t) * cnt, MALLOC_CAP_8BIT);
        if (filter) {
          float lp_fc = filterParams.fc_1 / samplerate;
          float lp_gain = filterParams.gain_1;
          float hp_fc = filterParams.fc_3 / samplerate;
          float hp_gain = filterParams.gain_3;

          filter[0] = (ptype_t){LPF,  lp_fc, lp_gain,         0.707,
                                NULL, NULL,  {0, 0, 0, 0, 0}, {0, 0}};
          filter[1] = (ptype_t){LPF,  lp_fc, lp_gain,         0.707,
                                NULL, NULL,  {0, 0, 0, 0, 0}, {0, 0}};
          filter[2] = (ptype_t){HPF,  hp_fc, hp_gain,         0.707,
                                NULL, NULL,  {0, 0, 0, 0, 0}, {0, 0}};
          filter[3] = (ptype_t){HPF,  hp_fc, hp_gain,         0.707,
                                NULL, NULL,  {0, 0, 0, 0, 0}, {0, 0}};

          ESP_LOGI(TAG, "got new setting for dspfBiamp");
        } else {
          ESP_LOGE(TAG, "failed to get memory for filter");
        }

        break;
      }

      case dspf2DOT1: {  // Process audio L + R LOW PASS FILTER
        cnt = 0;
        dspFlow = dspfStereo;

        ESP_LOGW(TAG, "dspf2DOT1, not implemented yet, using stereo instead");
      } break;

      case dspfFunkyHonda: {  // Process audio L + R LOW PASS FILTER
        cnt = 0;
        dspFlow = dspfStereo;

        ESP_LOGW(TAG,
                 "dspfFunkyHonda, not implemented yet, using stereo instead");
        break;
      }

      default: { break; }
    }

    dsp_processor_gen_filter(filter, cnt);

    init = true;
  }

  // only process data if it is valid
  if (audio_tmp) {
    sbuffer0 = (float *)heap_caps_malloc(sizeof(float) * DSP_PROCESSOR_LEN,
                                         MALLOC_CAP_8BIT);
    if (sbuffer0 == NULL) {
      ESP_LOGE(TAG, "No Memory allocated for dsp_processor sbuffer0");

      return -1;
    }

    sbufout0 = (float *)heap_caps_malloc(sizeof(float) * DSP_PROCESSOR_LEN,
                                         MALLOC_CAP_8BIT);
    if (sbufout0 == NULL) {
      ESP_LOGE(TAG, "No Memory allocated for dsp_processor sbufout0");

      free(sbuffer0);

      return -1;
    }

    switch (dspFlow) {
      case dspfEQBassTreble: {
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);
          uint32_t max = DSP_PROCESSOR_LEN;
          uint32_t test = len - k;

          if (test < DSP_PROCESSOR_LEN) {
            max = test;
          }

          // channel 0
          for (i = 0; i < max; i++) {
            sbuffer0[i] = dynamic_vol * /*0.5 **/
                          ((float)((int16_t)(tmp[i] & 0xFFFF))) / INT16_MAX;
          }

          // BASS
          BIQUAD(sbuffer0, sbufout0, max, filter[0].coeffs, filter[0].w);
          // TREBLE
          BIQUAD(sbufout0, sbuffer0, max, filter[1].coeffs, filter[1].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbuffer0[i] * INT16_MAX);
            tmp[i] =
                (volatile uint32_t)((tmp[i] & 0xFFFF0000) + (uint32_t)valint);
          }

          // channel 1
          for (i = 0; i < max; i++) {
            sbuffer0[i] = dynamic_vol * /*0.5 **/
                          ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))) /
                          INT16_MAX;
          }

          // BASS
          BIQUAD(sbuffer0, sbufout0, max, filter[2].coeffs, filter[2].w);
          // TREBLE
          BIQUAD(sbufout0, sbuffer0, max, filter[3].coeffs, filter[3].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbuffer0[i] * INT16_MAX);
            tmp[i] = (volatile uint32_t)((tmp[i] & 0xFFFF) +
                                         ((uint32_t)valint << 16));
          }
        }

        break;
      }

      case dspfStereo: {
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);
          uint32_t max = DSP_PROCESSOR_LEN;
          uint32_t test = len - k;

          if (test < DSP_PROCESSOR_LEN) {
            max = test;
          }

          // set volume
          if (dynamic_vol != 1.0) {
            for (i = 0; i < max; i++) {
              tmp[i] =
                  ((uint32_t)(dynamic_vol *
                              ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))))
                   << 16) +
                  (uint32_t)(dynamic_vol *
                             ((float)((int16_t)(tmp[i] & 0xFFFF))));
            }
          }
        }

        break;
      }

      case dspfBassBoost: {  // CH0 low shelf 6dB @ 400Hz
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);
          uint32_t max = DSP_PROCESSOR_LEN;
          uint32_t test = len - k;

          if (test < DSP_PROCESSOR_LEN) {
            max = test;
          }

          // channel 0
          for (i = 0; i < max; i++) {
            sbuffer0[i] = dynamic_vol * 0.5 *
                          ((float)((int16_t)(tmp[i] & 0xFFFF))) / INT16_MAX;
          }
          BIQUAD(sbuffer0, sbufout0, max, filter[0].coeffs, filter[0].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbufout0[i] * INT16_MAX);
            tmp[i] = (tmp[i] & 0xFFFF0000) + (uint32_t)valint;
          }

          // channel 1
          for (i = 0; i < max; i++) {
            sbuffer0[i] = dynamic_vol * 0.5 *
                          ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))) /
                          INT16_MAX;
          }
          BIQUAD(sbuffer0, sbufout0, max, filter[1].coeffs, filter[1].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbufout0[i] * INT16_MAX);
            tmp[i] = (tmp[i] & 0xFFFF) + ((uint32_t)valint << 16);
          }
        }

        break;
      }

      case dspfBiamp: {
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);
          uint32_t max = DSP_PROCESSOR_LEN;
          uint32_t test = len - k;

          if (test < DSP_PROCESSOR_LEN) {
            max = test;
          }

          // Process audio ch0 LOW PASS FILTER
          for (i = 0; i < max; i++) {
            sbuffer0[i] = dynamic_vol * 0.5 *
                          ((float)((int16_t)(tmp[i] & 0xFFFF))) / INT16_MAX;
          }
          BIQUAD(sbuffer0, sbufout0, max, filter[0].coeffs, filter[0].w);
          BIQUAD(sbufout0, sbuffer0, max, filter[1].coeffs, filter[1].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbuffer0[i] * INT16_MAX);
            tmp[i] = (tmp[i] & 0xFFFF0000) + (uint32_t)valint;
          }

          // Process audio ch1 HIGH PASS FILTER
          for (i = 0; i < max; i++) {
            sbuffer0[i] = dynamic_vol * 0.5 *
                          ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))) /
                          INT16_MAX;
          }
          BIQUAD(sbuffer0, sbufout0, max, filter[2].coeffs, filter[2].w);
          BIQUAD(sbufout0, sbuffer0, max, filter[3].coeffs, filter[3].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbuffer0[i] * INT16_MAX);
            tmp[i] = (tmp[i] & 0xFFFF) + ((uint32_t)valint << 16);
          }
        }

        break;
      }

      case dspf2DOT1: {  // Process audio L + R LOW PASS FILTER
        /*
           BIQUAD(sbuffer2, sbuftmp0, len, bq[0].coeffs, bq[0].w);
           BIQUAD(sbuftmp0, sbufout2, len, bq[1].coeffs, bq[1].w);

           // Process audio L HIGH PASS FILTER
           BIQUAD(sbuffer0, sbuftmp0, len, bq[2].coeffs, bq[2].w);
           BIQUAD(sbuftmp0, sbufout0, len, bq[3].coeffs, bq[3].w);

           // Process audio R HIGH PASS FILTER
           BIQUAD(sbuffer1, sbuftmp0, len, bq[4].coeffs, bq[4].w);
           BIQUAD(sbuftmp0, sbufout1, len, bq[5].coeffs, bq[5].w);

           int16_t valint[5];
           for (uint16_t i = 0; i < len; i++) {
             valint[0] =
                 (muteCH[0] == 1) ? (int16_t)0 : (int16_t)(sbufout0[i] *
           INT16_MAX); valint[1] = (muteCH[1] == 1) ? (int16_t)0 :
           (int16_t)(sbufout1[i] * INT16_MAX); valint[2] = (muteCH[2] == 1) ?
           (int16_t)0 : (int16_t)(sbufout2[i] * INT16_MAX); dsp_audio[i * 4 + 0]
           = (valint[2] & 0xff); dsp_audio[i * 4 + 1] = ((valint[2] & 0xff00) >>
           8); dsp_audio[i * 4 + 2] = 0; dsp_audio[i * 4 + 3] = 0;

             dsp_audio1[i * 4 + 0] = (valint[0] & 0xff);
             dsp_audio1[i * 4 + 1] = ((valint[0] & 0xff00) >> 8);
             dsp_audio1[i * 4 + 2] = (valint[1] & 0xff);
             dsp_audio1[i * 4 + 3] = ((valint[1] & 0xff00) >> 8);
           }

           // TODO: this copy could be avoided if dsp_audio buffers are
           // allocated dynamically and pointers are exchanged after
           // audio was freed
           memcpy(audio, dsp_audio, chunk_size);

           ESP_LOGW(TAG, "Don't know what to do with dsp_audio1");
     */
        ESP_LOGW(TAG, "dspf2DOT1, not implemented yet, using stereo instead");
      } break;

      case dspfFunkyHonda: {  // Process audio L + R LOW PASS FILTER
        /*
          BIQUAD(sbuffer2, sbuftmp0, len, bq[0].coeffs, bq[0].w);
          BIQUAD(sbuftmp0, sbufout2, len, bq[1].coeffs, bq[1].w);

          // Process audio L HIGH PASS FILTER
          BIQUAD(sbuffer0, sbuftmp0, len, bq[2].coeffs, bq[2].w);
          BIQUAD(sbuftmp0, sbufout0, len, bq[3].coeffs, bq[3].w);

          // Process audio R HIGH PASS FILTER
          BIQUAD(sbuffer1, sbuftmp0, len, bq[4].coeffs, bq[4].w);
          BIQUAD(sbuftmp0, sbufout1, len, bq[5].coeffs, bq[5].w);

          uint16_t scale = 16384;  // INT16_MAX
          int16_t valint[5];
          for (uint16_t i = 0; i < len; i++) {
            valint[0] =
                (muteCH[0] == 1) ? (int16_t)0 : (int16_t)(sbufout0[i] * scale);
            valint[1] =
                (muteCH[1] == 1) ? (int16_t)0 : (int16_t)(sbufout1[i] * scale);
            valint[2] =
                (muteCH[2] == 1) ? (int16_t)0 : (int16_t)(sbufout2[i] * scale);
            valint[3] = valint[0] + valint[2];
            valint[4] = -valint[2];
            valint[5] = -valint[1] - valint[2];
            dsp_audio[i * 4 + 0] = (valint[3] & 0xff);
            dsp_audio[i * 4 + 1] = ((valint[3] & 0xff00) >> 8);
            dsp_audio[i * 4 + 2] = (valint[2] & 0xff);
            dsp_audio[i * 4 + 3] = ((valint[2] & 0xff00) >> 8);

            dsp_audio1[i * 4 + 0] = (valint[4] & 0xff);
            dsp_audio1[i * 4 + 1] = ((valint[4] & 0xff00) >> 8);
            dsp_audio1[i * 4 + 2] = (valint[5] & 0xff);
            dsp_audio1[i * 4 + 3] = ((valint[5] & 0xff00) >> 8);
          }

          // TODO: this copy could be avoided if dsp_audio buffers are
          // allocated dynamically and pointers are exchanged after
          // audio was freed
          memcpy(audio, dsp_audio, chunk_size);

          ESP_LOGW(TAG, "Don't know what to do with dsp_audio1");
          */
        ESP_LOGW(TAG,
                 "dspfFunkyHonda, not implemented yet, using stereo instead");
      } break;

      default: { } break; }

    free(sbuffer0);
    sbuffer0 = NULL;

    free(sbufout0);
    sbufout0 = NULL;
  }

  return 0;
}

// void dsp_set_xoverfreq(uint8_t freqh, uint8_t freql, uint32_t samplerate) {
//  float freq = freqh * 256 + freql;
//  //  printf("%f\n", freq);
//  float f = freq / samplerate / 2.;
//  for (int8_t n = 0; n <= 5; n++) {
//    bq[n].freq = f;
//    switch (bq[n].filtertype) {
//      case LPF:
//        //        for (uint8_t i = 0; i <= 4; i++) {
//        //          printf("%.6f ", bq[n].coeffs[i]);
//        //        }
//        //        printf("\n");
//        dsps_biquad_gen_lpf_f32(bq[n].coeffs, bq[n].freq, bq[n].q);
//        //        for (uint8_t i = 0; i <= 4; i++) {
//        //          printf("%.6f ", bq[n].coeffs[i]);
//        //        }
//        //        printf("%f \n", bq[n].freq);
//        break;
//      case HPF:
//        dsps_biquad_gen_hpf_f32(bq[n].coeffs, bq[n].freq, bq[n].q);
//        break;
//      default:
//        break;
//    }
//  }
//}

/**
 *
 */
void dsp_processor_set_volome(double volume) {
  if (volume >= 0 && volume <= 1.0) {
    // Apply 2.5 volume curve to reduce loudness at low percentages
    double raw_volume = pow(volume, 2.5);
    dynamic_vol_base = raw_volume;

    // Apply gain boost to match A2DP loudness levels (runtime configurable)
    float gain_boost = get_runtime_gain_boost();
    dynamic_vol = raw_volume * gain_boost;

    ESP_LOGI(TAG, "Set volume to %f (raw: %f, boosted: %f, boost %fx)",
             volume, dynamic_vol_base, dynamic_vol, (double)gain_boost);

    // Check if any dynamic EQ gain has changed significantly and update filters if needed
    bool needs_update = false;
    
    if (dynamic_bass_enabled && filterParams.dspFlow == dspfEQBassTreble) {
      float current_gain = bass_low_gain + (bass_high_gain - bass_low_gain) * dynamic_vol_base;
      if (fabsf(current_gain - last_dynamic_bass_gain) >= 1.5f) {
        last_dynamic_bass_gain = current_gain;
        needs_update = true;
      }
    }

    if (dynamic_treble_enabled && filterParams.dspFlow == dspfEQBassTreble) {
      float current_gain = treble_low_gain + (treble_high_gain - treble_low_gain) * dynamic_vol_base;
      if (fabsf(current_gain - last_dynamic_treble_gain) >= 1.5f) {
        last_dynamic_treble_gain = current_gain;
        needs_update = true;
      }
    }

    if (needs_update) {
      // Trigger filter update for all three bands
      filterParams_t updateParams = filterParams;
      xQueueOverwrite(filterUpdateQHdl, &updateParams);
    }

    // Note: We don't re-init filters here as volume changes are frequent
    // and filter re-init is expensive. The bass gain is calculated dynamically
    // in the filter setup based on dynamic_vol_base.
  }
}
// --- Dynamic Bass Mapping Implementation ---
void dsp_processor_set_dynamic_bass(bool enabled) {
    dynamic_bass_enabled = enabled;
    init = false; // reinit filters so new mode is effective immediately
}

void dsp_processor_set_bass_mapping(float low_gain, float high_gain) {
    bass_low_gain = low_gain;
    bass_high_gain = high_gain;
    init = false; // reinit filters so settings are effective immediately
}

bool dsp_processor_get_dynamic_bass_enabled(void) {
    return dynamic_bass_enabled;
}

void dsp_processor_get_bass_mapping(float *low_gain, float *high_gain) {
    if (low_gain) *low_gain = bass_low_gain;
    if (high_gain) *high_gain = bass_high_gain;
}

void dsp_processor_set_dynamic_treble(bool enabled) {
    dynamic_treble_enabled = enabled;
    init = false; // reinit filters so new mode is effective immediately
}

void dsp_processor_set_treble_mapping(float low_gain, float high_gain) {
    treble_low_gain = low_gain;
    treble_high_gain = high_gain;
    init = false; // reinit filters so settings are effective immediately
}

bool dsp_processor_get_dynamic_treble_enabled(void) {
    return dynamic_treble_enabled;
}

void dsp_processor_get_treble_mapping(float *low_gain, float *high_gain) {
    if (low_gain) *low_gain = treble_low_gain;
    if (high_gain) *high_gain = treble_high_gain;
}
#endif
