#include "led_controller.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static const char *TAG = "LED_CTRL";

// WS2812B timing (in nanoseconds for RMT)
#define WS2812_T0H_NS 350
#define WS2812_T0L_NS 900
#define WS2812_T1H_NS 900
#define WS2812_T1L_NS 350
#define WS2812_RESET_US 280

// Audio processing
#define AUDIO_SAMPLE_RATE 48000
#define FFT_SIZE 64
#define AUDIO_HISTORY_SIZE 512
#define NUM_SPECTRUM_BANDS 6
// Consider audio "silent" when RMS is below this for a while
#define SILENCE_THRESHOLD 0.005f
#define SILENCE_HOLD_FRAMES 40

static led_config_t g_config;
static uint8_t *g_led_buffer = NULL;  // RGB buffer
static rmt_channel_handle_t g_led_chan = NULL;
static rmt_encoder_handle_t g_led_encoder = NULL;
static TaskHandle_t g_led_task = NULL;
static SemaphoreHandle_t g_config_mutex = NULL;

// Audio analysis variables
static float g_audio_energy = 0;
static float g_bass_energy = 0;
static float g_mid_energy = 0;
static float g_treble_energy = 0;
static float g_peak_level = 0;
static uint32_t g_frame_count = 0;

// Simple moving average for smoothing
static float g_energy_avg = 0;
static float g_bass_avg = 0;
static float g_band_energy[NUM_SPECTRUM_BANDS] = {0};
static float g_band_avg[NUM_SPECTRUM_BANDS] = {0};
// For equalizer-style peak hold
static float g_band_peak[NUM_SPECTRUM_BANDS] = {0};

// Cache for solid-color mode so we don't resend frames unnecessarily
static bool g_solid_cached = false;
static led_color_t g_solid_color;
static uint8_t g_solid_brightness = 0;
static bool g_is_silent = false;
static uint32_t g_silence_frames = 0;
static uint16_t g_auto_rainbow_hue = 0;
static uint16_t g_auto_rainbow2_hue = 0;
static uint64_t g_last_audio_time_us = 0;

// WS2812B byte encoder structure
typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} rmt_led_strip_encoder_t;

// RMT encoder functions
static size_t rmt_encode_led_strip(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                   const void *primary_data, size_t data_size,
                                   rmt_encode_state_t *ret_state)
{
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;
    
    switch (led_encoder->state) {
    case 0: // send RGB data
        encoded_symbols += led_encoder->bytes_encoder->encode(led_encoder->bytes_encoder, channel,
                                                              primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = 1; // switch to next state when current encoding session finished
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
    // fall-through
    case 1: // send reset code
        encoded_symbols += led_encoder->copy_encoder->encode(led_encoder->copy_encoder, channel,
                                                             &led_encoder->reset_code,
                                                             sizeof(led_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = RMT_ENCODING_RESET; // back to the initial encoding session
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
    }
out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t rmt_del_led_strip_encoder(rmt_encoder_t *encoder)
{
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_del_encoder(led_encoder->bytes_encoder);
    rmt_del_encoder(led_encoder->copy_encoder);
    free(led_encoder);
    return ESP_OK;
}

static esp_err_t rmt_led_strip_encoder_reset(rmt_encoder_t *encoder)
{
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    led_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t rmt_new_led_strip_encoder(rmt_encoder_handle_t *ret_encoder)
{
    esp_err_t ret = ESP_OK;
    rmt_led_strip_encoder_t *led_encoder = NULL;
    
    led_encoder = calloc(1, sizeof(rmt_led_strip_encoder_t));
    if (!led_encoder) {
        return ESP_ERR_NO_MEM;
    }
    
    led_encoder->base.encode = rmt_encode_led_strip;
    led_encoder->base.del = rmt_del_led_strip_encoder;
    led_encoder->base.reset = rmt_led_strip_encoder_reset;
    
    // WS2812 uses RGB format, MSB first
    // Resolution is 10MHz = 100ns per tick
    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = WS2812_T0H_NS / 100, // 350ns / 100ns = 3-4 ticks
            .level1 = 0,
            .duration1 = WS2812_T0L_NS / 100, // 900ns / 100ns = 9 ticks
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = WS2812_T1H_NS / 100, // 900ns / 100ns = 9 ticks
            .level1 = 0,
            .duration1 = WS2812_T1L_NS / 100, // 350ns / 100ns = 3-4 ticks
        },
        .flags.msb_first = 1,
    };
    ret = rmt_new_bytes_encoder(&bytes_encoder_config, &led_encoder->bytes_encoder);
    if (ret != ESP_OK) {
        goto err;
    }
    
    rmt_copy_encoder_config_t copy_encoder_config = {};
    ret = rmt_new_copy_encoder(&copy_encoder_config, &led_encoder->copy_encoder);
    if (ret != ESP_OK) {
        goto err;
    }
    
    // Reset time: 280us at 10MHz = 280 * 10 = 2800 ticks
    uint32_t reset_ticks = (WS2812_RESET_US * 10); // 280us * 10 ticks/us = 2800 ticks
    led_encoder->reset_code = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = reset_ticks,
        .level1 = 0,
        .duration1 = reset_ticks,
    };
    
    *ret_encoder = &led_encoder->base;
    return ESP_OK;
    
err:
    if (led_encoder) {
        if (led_encoder->bytes_encoder) {
            rmt_del_encoder(led_encoder->bytes_encoder);
        }
        if (led_encoder->copy_encoder) {
            rmt_del_encoder(led_encoder->copy_encoder);
        }
        free(led_encoder);
    }
    return ret;
}

// Helper: Apply brightness to color
static inline void apply_brightness(uint8_t *r, uint8_t *g, uint8_t *b, uint8_t brightness)
{
    *r = (*r * brightness) >> 8;
    *g = (*g * brightness) >> 8;
    *b = (*b * brightness) >> 8;
}

// Helper: HSV to RGB conversion
static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region, remainder, p, q, t;
    
    if (s == 0) {
        *r = v;
        *g = v;
        *b = v;
        return;
    }
    
    region = h / 43;
    remainder = (h - (region * 43)) * 6;
    
    p = (v * (255 - s)) >> 8;
    q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
    
    switch (region) {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

// Helper: Scale normalized value [0,1] by sensitivity slider
// Normal mode: ~0.5x..4x gain. When ignore_volume is enabled,
// allow a bit more headroom so LEDs stay punchy even at nominal levels.
static float scale_by_sensitivity(float value)
{
    float sens = g_config.sensitivity / 255.0f;  // 0.0 - 1.0

    // Base gain curve
    float min_gain = 0.5f;
    float max_gain = 4.0f;

    // When ignoring playback volume, boost the effective range a bit
    // so max sensitivity feels closer to "fully cranked" even though
    // the audio is kept at a sane nominal level.
    if (g_config.ignore_volume) {
        min_gain = 0.7f;
        max_gain = 6.0f;
    }

    float gain = min_gain + sens * (max_gain - min_gain);
    float scaled = value * gain;
    if (scaled > 1.0f) scaled = 1.0f;
    if (scaled < 0.0f) scaled = 0.0f;
    return scaled;
}

// Helper: Blend between full-spectrum and bass-focused energy based on
// the global bass_focus slider (0 = full-spectrum, 255 = bass-focused).
// Most effects that previously used overall g_energy_avg should instead
// call this helper so the user can tilt visuals toward sub-bass.
static float get_focused_energy(void)
{
    float focus = g_config.bass_focus / 255.0f; // 0..1
    float mixed = (1.0f - focus) * g_energy_avg + focus * g_bass_avg;
    return scale_by_sensitivity(mixed);
}

// Helper: Estimate sub-bass energy using a single-bin Goertzel-like analysis
// over the current audio frame. This gives a compact "small FFT" focused
// on low frequencies (around 60 Hz) without the full cost of a full spectrum.
static float compute_sub_bass_energy(const int16_t *audio_data, size_t num_samples)
{
    if (!audio_data || num_samples == 0) {
        return 0.0f;
    }

    const float target_hz = 60.0f; // focus band for kick/sub
    float kf = (float)num_samples * target_hz / (float)AUDIO_SAMPLE_RATE;
    int k = (int)(kf + 0.5f);
    if (k < 1) k = 1;
    if (k > (int)num_samples - 1) k = (int)num_samples - 1;

    float omega = 2.0f * (float)M_PI * (float)k / (float)num_samples;
    float coeff = 2.0f * cosf(omega);
    float q0 = 0.0f, q1 = 0.0f, q2 = 0.0f;

    // Treat incoming stereo as mono by averaging L/R
    for (size_t i = 0; i < num_samples * 2; i += 2) {
        float sample = (audio_data[i] + audio_data[i + 1]) / 2.0f / 32768.0f;
        q0 = coeff * q1 - q2 + sample;
        q2 = q1;
        q1 = q0;
    }

    float real = q1 - q2 * cosf(omega);
    float imag = q2 * sinf(omega);
    float mag2 = real * real + imag * imag;

    // Rough normalization to a 0..1-ish range
    float norm = (float)num_samples;
    if (norm > 0.0f) {
        mag2 /= (norm * norm);
    }

    float mag = sqrtf(mag2);
    if (mag > 1.0f) mag = 1.0f;
    if (mag < 0.0f) mag = 0.0f;
    return mag;
}

// Helper: Set single LED in buffer (supports RGB/GRB order)
static void set_led(uint16_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index >= g_config.num_leds || !g_led_buffer) return;
    
    uint32_t offset = index * 3;
    if (g_config.color_order == 1) {
        // GRB order (common for many WS2812B strips)
        g_led_buffer[offset + 0] = g;
        g_led_buffer[offset + 1] = r;
        g_led_buffer[offset + 2] = b;
    } else {
        // RGB order
        g_led_buffer[offset + 0] = r;
        g_led_buffer[offset + 1] = g;
        g_led_buffer[offset + 2] = b;
    }
}

// Helper: Clear all LEDs
static void clear_leds(void)
{
    if (g_led_buffer) {
        memset(g_led_buffer, 0, g_config.num_leds * 3);
    }
}

// Update LED strip via RMT
static esp_err_t update_leds(void)
{
    if (!g_led_chan || !g_led_encoder || !g_led_buffer) {
        return ESP_ERR_INVALID_STATE;
    }
    
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    
    return rmt_transmit(g_led_chan, g_led_encoder, g_led_buffer, 
                       g_config.num_leds * 3, &tx_config);
}

// Effect: VU Meter (single bar)
static void effect_vu_meter(void)
{
    clear_leds();
    
    float level = get_focused_energy();
    int lit_leds = (int)(level * g_config.num_leds);
    if (lit_leds > g_config.num_leds) lit_leds = g_config.num_leds;
    
    for (int i = 0; i < lit_leds; i++) {
        float ratio = (float)i / g_config.num_leds;
        uint8_t r, g, b;
        
        if (ratio < 0.5f) {
            // Green to yellow
            r = (uint8_t)(ratio * 2.0f * 255);
            g = 255;
            b = 0;
        } else {
            // Yellow to red
            r = 255;
            g = (uint8_t)((1.0f - ratio) * 2.0f * 255);
            b = 0;
        }
        
        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(i, r, g, b);
    }
}

// Effect: Stereo VU (left and right from center)
static void effect_stereo_vu(void)
{
    clear_leds();
    
    float energy = get_focused_energy();
    int mid = g_config.num_leds / 2;
    int lit = (int)(energy * mid);
    if (lit > mid) lit = mid;
    
    for (int i = 0; i < lit; i++) {
        float ratio = (float)i / mid;
        uint8_t r = (uint8_t)(ratio * 255);
        uint8_t g = (uint8_t)((1.0f - ratio) * 255);
        uint8_t b = 0;
        
        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(mid - i - 1, r, g, b);  // Left
        set_led(mid + i, r, g, b);       // Right
    }
}

// Effect: Spectrum analyzer (6 bands using simple time-domain slices)
static void effect_spectrum(void)
{
    clear_leds();

    int bands = NUM_SPECTRUM_BANDS;
    int segment = (bands > 0) ? (g_config.num_leds / bands) : 0;
    if (segment <= 0) {
        segment = 1;
    }

    for (int band = 0; band < bands; band++) {
        int base_index = band * segment;
        if (base_index >= g_config.num_leds) {
            break;
        }

        float level = scale_by_sensitivity(g_band_avg[band]);
        int lit_leds = (int)(level * segment);
        if (lit_leds > segment) lit_leds = segment;

        uint8_t r = 0, g = 0, b = 0;
        switch (band) {
            case 0: // deep bass - red
                r = 255; g = 0;   b = 0;   break;
            case 1: // bass - orange
                r = 255; g = 128; b = 0;   break;
            case 2: // low mids - yellow
                r = 255; g = 255; b = 0;   break;
            case 3: // mids - green
                r = 0;   g = 255; b = 0;   break;
            case 4: // high mids - cyan
                r = 0;   g = 180; b = 255; break;
            default: // treble - purple
                r = 180; g = 0;   b = 255; break;
        }

        for (int i = 0; i < lit_leds; i++) {
            int idx = base_index + i;
            if (idx >= g_config.num_leds) {
                break;
            }
            uint8_t cr = r, cg = g, cb = b;
            apply_brightness(&cr, &cg, &cb, g_config.brightness);
            set_led(idx, cr, cg, cb);
        }
    }
}

// Effect: Equalizer with peak hold per band
static void effect_equalizer(void)
{
    clear_leds();

    int bands = NUM_SPECTRUM_BANDS;
    int segment = (bands > 0) ? (g_config.num_leds / bands) : 0;
    if (segment <= 0) {
        segment = 1;
    }

    for (int band = 0; band < bands; band++) {
        int base_index = band * segment;
        if (base_index >= g_config.num_leds) {
            break;
        }

        float level = scale_by_sensitivity(g_band_avg[band]);
        if (level < 0.0f) level = 0.0f;
        if (level > 1.0f) level = 1.0f;

        // Update peak hold with slow decay
        if (level > g_band_peak[band]) {
            g_band_peak[band] = level;
        } else {
            g_band_peak[band] *= 0.94f;
            if (g_band_peak[band] < 0.0f) g_band_peak[band] = 0.0f;
        }

        int lit_leds = (int)(level * segment);
        if (lit_leds > segment) lit_leds = segment;

        // Bar color blends between primary and secondary based on band index
        float t_band = (bands > 1) ? ((float)band / (float)(bands - 1)) : 0.0f;
        uint8_t base_r = (uint8_t)((1.0f - t_band) * g_config.color1.r + t_band * g_config.color2.r);
        uint8_t base_g = (uint8_t)((1.0f - t_band) * g_config.color1.g + t_band * g_config.color2.g);
        uint8_t base_b = (uint8_t)((1.0f - t_band) * g_config.color1.b + t_band * g_config.color2.b);

        for (int i = 0; i < lit_leds; i++) {
            int idx = base_index + i;
            if (idx >= g_config.num_leds) {
                break;
            }

            // Slightly fade towards the top of the bar
            float rel = (float)(i + 1) / (float)segment;
            float level_scale = 0.4f + 0.6f * (1.0f - rel);

            uint8_t r = (uint8_t)(base_r * level_scale);
            uint8_t g = (uint8_t)(base_g * level_scale);
            uint8_t b = (uint8_t)(base_b * level_scale);

            apply_brightness(&r, &g, &b, g_config.brightness);
            set_led(idx, r, g, b);
        }

        // Draw peak marker as a bright pixel using secondary color / white-ish
        int peak_pos = (int)(g_band_peak[band] * segment);
        if (peak_pos > segment - 1) peak_pos = segment - 1;
        if (peak_pos >= 0) {
            int idx = base_index + peak_pos;
            if (idx < g_config.num_leds) {
                uint8_t r = g_config.color2.r;
                uint8_t g = g_config.color2.g;
                uint8_t b = g_config.color2.b;
                // Make peak a bit brighter
                r = (uint8_t)((int)r + 80 > 255 ? 255 : r + 80);
                g = (uint8_t)((int)g + 80 > 255 ? 255 : g + 80);
                b = (uint8_t)((int)b + 80 > 255 ? 255 : b + 80);

                apply_brightness(&r, &g, &b, g_config.brightness);
                set_led(idx, r, g, b);
            }
        }
    }
}

// Effect: Pulse
static void effect_pulse(void)
{
    static uint8_t pulse_phase = 0;
    
    float energy = get_focused_energy();
    uint8_t intensity = (uint8_t)(sinf(pulse_phase * 0.05f) * 127 + 128);
    intensity = (intensity * energy * 2.0f);
    if (intensity > 255) intensity = 255;
    
    // Blend between primary and secondary based on pulse phase
    float t = (sinf(pulse_phase * 0.05f) + 1.0f) * 0.5f; // 0..1
    uint8_t r = (uint8_t)((1.0f - t) * g_config.color1.r + t * g_config.color2.r);
    uint8_t g = (uint8_t)((1.0f - t) * g_config.color1.g + t * g_config.color2.g);
    uint8_t b = (uint8_t)((1.0f - t) * g_config.color1.b + t * g_config.color2.b);
    
    r = (r * intensity) >> 8;
    g = (g * intensity) >> 8;
    b = (b * intensity) >> 8;
    
    apply_brightness(&r, &g, &b, g_config.brightness);
    
    for (int i = 0; i < g_config.num_leds; i++) {
        set_led(i, r, g, b);
    }
    
    pulse_phase += g_config.speed / 10 + 1;
}

// Effect: Wave
static void effect_wave(void)
{
    static uint16_t wave_pos = 0;
    
    float energy = get_focused_energy();
    
    for (int i = 0; i < g_config.num_leds; i++) {
        float phase = (wave_pos + i * 10) * 0.05f;
        uint8_t intensity = (uint8_t)(sinf(phase) * 127 + 128);
        intensity = (intensity * energy * 2.0f);
        if (intensity > 255) intensity = 255;
        
        // Position-based blend between primary and secondary
        float x = (g_config.num_leds > 1) ? ((float)i / (float)(g_config.num_leds - 1)) : 0.0f;
        uint8_t base_r = (uint8_t)((1.0f - x) * g_config.color1.r + x * g_config.color2.r);
        uint8_t base_g = (uint8_t)((1.0f - x) * g_config.color1.g + x * g_config.color2.g);
        uint8_t base_b = (uint8_t)((1.0f - x) * g_config.color1.b + x * g_config.color2.b);

        uint8_t r = (base_r * intensity) >> 8;
        uint8_t g = (base_g * intensity) >> 8;
        uint8_t b = (base_b * intensity) >> 8;
        
        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(i, r, g, b);
    }
    
    wave_pos += g_config.speed / 5 + 1;
}

// Effect: Energy bar
static void effect_energy_bar(void)
{
    static float decay = 0;
    
    float energy = get_focused_energy();
    if (energy > decay) {
        decay = energy;
    } else {
        decay *= 0.95f;  // Decay
    }
    
    int lit = (int)(decay * g_config.num_leds);
    if (lit > g_config.num_leds) lit = g_config.num_leds;
    
    clear_leds();
    for (int i = 0; i < lit; i++) {
        // Gradient from primary at start to secondary at tip
        float t = (lit > 1) ? ((float)i / (float)(lit - 1)) : 0.0f;
        uint8_t r = (uint8_t)((1.0f - t) * g_config.color1.r + t * g_config.color2.r);
        uint8_t g = (uint8_t)((1.0f - t) * g_config.color1.g + t * g_config.color2.g);
        uint8_t b = (uint8_t)((1.0f - t) * g_config.color1.b + t * g_config.color2.b);
        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(i, r, g, b);
    }
}

// Effect: Rainbow pulse
static void effect_rainbow_pulse(void)
{
    static uint16_t hue = 0;
    
    float energy = get_focused_energy();
    uint8_t value = (uint8_t)(energy * 255);
    if (value > 255) value = 255;
    
    for (int i = 0; i < g_config.num_leds; i++) {
        uint16_t pixel_hue = (hue + (i * 256 / g_config.num_leds)) % 256;
        uint8_t r, g, b;
        hsv_to_rgb(pixel_hue, 255, value, &r, &g, &b);
        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(i, r, g, b);
    }
    
    hue += g_config.speed / 20 + 1;
    if (hue >= 256) hue = 0;
}

// Effect: Beat flash
static void effect_beat_flash(void)
{
    static float flash_intensity = 0;
    static float last_energy = 0;
    static bool use_secondary = false;
    
    float energy = g_energy_avg;
    float delta = energy - last_energy;
    
    // Detect beat (sudden energy increase)
    // Note: higher slider sensitivity should make this EASIER to trigger
    float sens = g_config.sensitivity / 255.0f;      // 0..1
    float base_thresh = 0.3f;                        // default threshold
    float thresh = base_thresh * (1.0f - sens * 0.9f); // ~0.3 at sens=0, ~0.03 at sens=255
    if (delta > thresh) {
        flash_intensity = 1.0f;
        // Alternate between primary and secondary on each detected beat
        use_secondary = !use_secondary;
    }
    
    flash_intensity *= 0.85f;  // Decay
    last_energy = energy * 0.8f + last_energy * 0.2f;
    
    uint8_t base_r = use_secondary ? g_config.color2.r : g_config.color1.r;
    uint8_t base_g = use_secondary ? g_config.color2.g : g_config.color1.g;
    uint8_t base_b = use_secondary ? g_config.color2.b : g_config.color1.b;

    uint8_t r = (uint8_t)(base_r * flash_intensity);
    uint8_t g = (uint8_t)(base_g * flash_intensity);
    uint8_t b = (uint8_t)(base_b * flash_intensity);
    
    apply_brightness(&r, &g, &b, g_config.brightness);
    
    for (int i = 0; i < g_config.num_leds; i++) {
        set_led(i, r, g, b);
    }
}

// Effect: Bass pulse
static void effect_bass_pulse(void)
{
    static float pulse_val = 0;
    
    // Use scaled bass band so this reacts strongly even on normal mixes
    float bass = scale_by_sensitivity(g_bass_avg);
    if (bass > pulse_val) {
        pulse_val = bass;
    } else {
        pulse_val *= 0.9f;
    }

    // Keep a small baseline when there is audio so it doesn't look "off"
    if (pulse_val < 0.05f && g_audio_energy > SILENCE_THRESHOLD) {
        pulse_val = 0.05f;
    }

    uint8_t intensity = (uint8_t)(pulse_val * 255);
    if (intensity > 255) intensity = 255;
    
    // As bass increases, blend from primary towards secondary
    float t = pulse_val;
    if (t > 1.0f) t = 1.0f;
    uint8_t base_r = (uint8_t)((1.0f - t) * g_config.color1.r + t * g_config.color2.r);
    uint8_t base_g = (uint8_t)((1.0f - t) * g_config.color1.g + t * g_config.color2.g);
    uint8_t base_b = (uint8_t)((1.0f - t) * g_config.color1.b + t * g_config.color2.b);

    uint8_t r = (base_r * intensity) >> 8;
    uint8_t g = (base_g * intensity) >> 8;
    uint8_t b = (base_b * intensity) >> 8;
    
    apply_brightness(&r, &g, &b, g_config.brightness);
    
    for (int i = 0; i < g_config.num_leds; i++) {
        set_led(i, r, g, b);
    }
}

// Effect: Solid color
static void effect_solid(void)
{
    uint8_t r = g_config.color1.r;
    uint8_t g = g_config.color1.g;
    uint8_t b = g_config.color1.b;
    
    apply_brightness(&r, &g, &b, g_config.brightness);
    
    for (int i = 0; i < g_config.num_leds; i++) {
        set_led(i, r, g, b);
    }
}

// Effect: Color wipe between two colors
static void effect_color_wipe(void)
{
    static int position = 0;

    float energy = get_focused_energy();
    if (energy < 0.05f) {
        energy = 0.05f; // keep a minimal glow
    }

    clear_leds();

    for (int i = 0; i < g_config.num_leds; i++) {
        uint8_t r, g, b;
        if (i <= position) {
            r = g_config.color1.r;
            g = g_config.color1.g;
            b = g_config.color1.b;
        } else {
            r = g_config.color2.r;
            g = g_config.color2.g;
            b = g_config.color2.b;
        }

        // Audio-reactive overall intensity
        uint8_t intensity = (uint8_t)(energy * 255);
        r = (r * intensity) >> 8;
        g = (g * intensity) >> 8;
        b = (b * intensity) >> 8;

        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(i, r, g, b);
    }

    int step = (g_config.speed / 32) + 1;
    position += step;
    if (position >= g_config.num_leds) {
        position = 0;
    }
}

// Effect: Dual-color flowing wave
static void effect_dual_color_wave(void)
{
    static uint16_t phase = 0;

    float energy = get_focused_energy();
    if (energy < 0.05f) {
        energy = 0.05f;
    }

    for (int i = 0; i < g_config.num_leds; i++) {
        float t = (sinf((phase + i * 10) * 0.03f) + 1.0f) * 0.5f; // 0..1

        // Mix between color1 and color2
        uint8_t r = (uint8_t)((g_config.color1.r * (1.0f - t)) + (g_config.color2.r * t));
        uint8_t g = (uint8_t)((g_config.color1.g * (1.0f - t)) + (g_config.color2.g * t));
        uint8_t b = (uint8_t)((g_config.color1.b * (1.0f - t)) + (g_config.color2.b * t));

        uint8_t intensity = (uint8_t)(energy * 255);
        r = (r * intensity) >> 8;
        g = (g * intensity) >> 8;
        b = (b * intensity) >> 8;

        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(i, r, g, b);
    }

    phase += (g_config.speed / 16) + 1;
}

// Effect: Comet with trailing tail
static void effect_comet(void)
{
    static int head = 0;

    float energy = get_focused_energy();
    if (energy < 0.05f) {
        energy = 0.05f;
    }

    clear_leds();

    int trail = (g_config.num_leds / 8) + (g_config.speed / 32);
    if (trail < 3) trail = 3;

    for (int i = 0; i < g_config.num_leds; i++) {
        int dist = abs(i - head);
        if (dist > trail) continue;

        float fade = 1.0f - ((float)dist / (float)trail);
        float intensity_f = energy * fade;
        if (intensity_f < 0.0f) intensity_f = 0.0f;
        if (intensity_f > 1.0f) intensity_f = 1.0f;

        uint8_t intensity = (uint8_t)(intensity_f * 255);

        // Head closer to primary, tail fades towards secondary
        float t = (trail > 0) ? ((float)dist / (float)trail) : 0.0f;
        uint8_t base_r = (uint8_t)((1.0f - t) * g_config.color1.r + t * g_config.color2.r);
        uint8_t base_g = (uint8_t)((1.0f - t) * g_config.color1.g + t * g_config.color2.g);
        uint8_t base_b = (uint8_t)((1.0f - t) * g_config.color1.b + t * g_config.color2.b);

        uint8_t r = (base_r * intensity) >> 8;
        uint8_t g = (base_g * intensity) >> 8;
        uint8_t b = (base_b * intensity) >> 8;

        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(i, r, g, b);
    }

    int step = (g_config.speed / 32) + 1;
    head += step;
    if (head >= g_config.num_leds) {
        head = 0;
    }
}

// Effect: Sparkle / confetti
static void effect_sparkle(void)
{
    float energy = get_focused_energy();

    // Base background: dim secondary color
    uint8_t base_r = (g_config.color2.r * 40) >> 8;
    uint8_t base_g = (g_config.color2.g * 40) >> 8;
    uint8_t base_b = (g_config.color2.b * 40) >> 8;
    apply_brightness(&base_r, &base_g, &base_b, g_config.brightness);

    for (int i = 0; i < g_config.num_leds; i++) {
        set_led(i, base_r, base_g, base_b);
    }

    if (energy <= 0.0f) {
        return;
    }

    // Number of sparkles scales with energy and speed
    int max_sparkles = g_config.num_leds / 4;
    if (max_sparkles < 1) max_sparkles = 1;
    float density = energy * (0.2f + (g_config.speed / 255.0f) * 0.8f);
    int sparkles = (int)(density * max_sparkles);
    if (sparkles < 1) sparkles = 1;

    for (int s = 0; s < sparkles; s++) {
        if (g_config.num_leds == 0) break;
        int idx = (int)(esp_random() % g_config.num_leds);
        uint8_t r = g_config.color1.r;
        uint8_t g = g_config.color1.g;
        uint8_t b = g_config.color1.b;
        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(idx, r, g, b);
    }
}

// Effect: Beat-driven rainbow (WLED-style)
static void effect_beat_rainbow(void)
{
    static uint16_t base_hue = 0;
    static float flash_intensity = 0.0f;
    static float last_energy = 0.0f;

    float energy = g_energy_avg;
    float delta = energy - last_energy;

    // Sensitivity-dependent beat threshold
    float sens = g_config.sensitivity / 255.0f;       // 0..1
    float base_thresh = 0.25f;                        // default threshold
    float thresh = base_thresh * (1.0f - sens * 0.8f); // ~0.25..~0.05

    if (delta > thresh) {
        // Strong beat: full flash and advance hue
        flash_intensity = 1.0f;
        base_hue = (base_hue + 20) & 0xFF; // wrap 0-255
    }

    // Decay flash over time
    flash_intensity *= 0.90f;
    if (flash_intensity < 0.02f) {
        flash_intensity = 0.02f; // keep slight ambient
    }

    last_energy = energy * 0.7f + last_energy * 0.3f;

    float level = scale_by_sensitivity(g_energy_avg);
    if (level < 0.1f) level = 0.1f;

    clear_leds();

    for (int i = 0; i < g_config.num_leds; i++) {
        uint16_t hue = (base_hue + (i * 256 / (g_config.num_leds ? g_config.num_leds : 1))) & 0xFF;
        uint8_t r, g, b;

        float v = flash_intensity * (0.2f + level * 0.8f); // 0.2..1 scaled by beat & energy
        if (v > 1.0f) v = 1.0f;
        uint8_t value = (uint8_t)(v * 255.0f);

        hsv_to_rgb(hue, 255, value, &r, &g, &b);
        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(i, r, g, b);
    }
}

// Effect: Multiple comets chasing along the strip
static void effect_multi_comet(void)
{
    static bool inited = false;
    static int heads[3];

    if (!inited) {
        int n = g_config.num_leds ? g_config.num_leds : 1;
        heads[0] = 0;
        heads[1] = n / 3;
        heads[2] = (2 * n) / 3;
        inited = true;
    }

    float energy = get_focused_energy();
    if (energy < 0.05f) {
        energy = 0.05f;
    }

    clear_leds();

    int n = g_config.num_leds;
    if (n <= 0) return;

    int trail = (n / 10) + (g_config.speed / 40);
    if (trail < 4) trail = 4;

    for (int i = 0; i < n; i++) {
        float total = 0.0f;
        for (int h = 0; h < 3; h++) {
            int dist = abs(i - heads[h]);
            if (dist > trail) continue;
            float fade = 1.0f - ((float)dist / (float)trail);
            if (fade > 0.0f) {
                total += energy * fade;
            }
        }

        if (total <= 0.0f) continue;
        if (total > 1.0f) total = 1.0f;

        uint8_t intensity = (uint8_t)(total * 255.0f);
        uint8_t r = (g_config.color1.r * intensity) >> 8;
        uint8_t g = (g_config.color1.g * intensity) >> 8;
        uint8_t b = (g_config.color1.b * intensity) >> 8;

        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(i, r, g, b);
    }

    int step = (g_config.speed / 32) + 1;
    for (int h = 0; h < 3; h++) {
        heads[h] += step;
        if (heads[h] >= n) {
            heads[h] -= n;
        }
    }
}

// Effect: Marching segments (alternating colors)
static void effect_marching(void)
{
    static uint16_t offset = 0;

    float energy = get_focused_energy();
    if (energy < 0.1f) energy = 0.1f;

    int n = g_config.num_leds;
    if (n <= 0) return;

    int segment = 3 + (g_config.speed / 32); // segment length
    clear_leds();

    for (int i = 0; i < n; i++) {
        int pos = (i + offset) / segment;
        bool use_primary = (pos % 2) == 0;

        uint8_t r = use_primary ? g_config.color1.r : g_config.color2.r;
        uint8_t g = use_primary ? g_config.color1.g : g_config.color2.g;
        uint8_t b = use_primary ? g_config.color1.b : g_config.color2.b;

        uint8_t intensity = (uint8_t)(energy * 255.0f);
        r = (r * intensity) >> 8;
        g = (g * intensity) >> 8;
        b = (b * intensity) >> 8;

        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(i, r, g, b);
    }

    int step = 1 + (g_config.speed / 32);
    offset = (offset + step) % (segment * 2);
}

// Effect: Blade power+ (center-out energy blade)
static void effect_blade_power(void)
{
    int n = g_config.num_leds;
    if (n <= 0) return;

    float energy = get_focused_energy();
    if (energy < 0.05f) energy = 0.05f;

    int mid = n / 2;
    int max_len = mid;
    int blade_len = (int)(energy * max_len);
    if (blade_len < 1) blade_len = 1;

    clear_leds();

    for (int i = 0; i <= blade_len && i < max_len; i++) {
        float t = (float)i / (float)blade_len; // 0 at center, 1 at tip
        if (t > 1.0f) t = 1.0f;

        // Gradient between color1 (base) and color2 (tip)
        uint8_t r = (uint8_t)((1.0f - t) * g_config.color1.r + t * g_config.color2.r);
        uint8_t g = (uint8_t)((1.0f - t) * g_config.color1.g + t * g_config.color2.g);
        uint8_t b = (uint8_t)((1.0f - t) * g_config.color1.b + t * g_config.color2.b);

        float fade = 1.0f - t * 0.5f;
        float level = energy * fade;
        if (level > 1.0f) level = 1.0f;
        uint8_t intensity = (uint8_t)(level * 255.0f);

        r = (r * intensity) >> 8;
        g = (g * intensity) >> 8;
        b = (b * intensity) >> 8;

        apply_brightness(&r, &g, &b, g_config.brightness);
        if (mid - i >= 0) set_led(mid - i, r, g, b);
        if (mid + i < n) set_led(mid + i, r, g, b);
    }
}

// Effect: Dual energy bar (mirrored from both ends)
static void effect_dual_energy_bar(void)
{
    static float decay = 0;

    int n = g_config.num_leds;
    if (n <= 0) return;

    float energy = get_focused_energy();
    if (energy > decay) {
        decay = energy;
    } else {
        decay *= 0.95f;
    }

    int half = n / 2;
    int lit = (int)(decay * half);
    if (lit > half) lit = half;

    clear_leds();

    for (int i = 0; i < lit; i++) {
        float t = (lit > 1) ? ((float)i / (float)(lit - 1)) : 0.0f;
        uint8_t r = (uint8_t)((1.0f - t) * g_config.color1.r + t * g_config.color2.r);
        uint8_t g = (uint8_t)((1.0f - t) * g_config.color1.g + t * g_config.color2.g);
        uint8_t b = (uint8_t)((1.0f - t) * g_config.color1.b + t * g_config.color2.b);
        apply_brightness(&r, &g, &b, g_config.brightness);

        if (half - 1 - i >= 0)      set_led(half - 1 - i, r, g, b);
        if (half + i < n)           set_led(half + i,     r, g, b);
    }
}

// Effect: Beat strobe (strong global flash on beats)
static void effect_charge_beam(void)
{
    static float flash_intensity = 0.0f;
    static float last_energy = 0.0f;
    static uint16_t base_hue = 0;

    int n = g_config.num_leds;
    if (n <= 0) return;

    float energy = g_energy_avg;
    float delta = energy - last_energy;

    float sens = g_config.sensitivity / 255.0f;       // 0..1
    float base_thresh = 0.25f;                        // default threshold
    float thresh = base_thresh * (1.0f - sens * 0.8f); // ~0.25..~0.05

    if (delta > thresh && energy > 0.02f) {
        // Strong beat detected: trigger full flash and advance hue
        flash_intensity = 1.0f;
        base_hue = (base_hue + 40) & 0xFF;
    }

    // Decay flash over time
    flash_intensity *= 0.85f;
    if (flash_intensity < 0.02f) {
        flash_intensity = 0.02f; // keep slight ambient
    }

    last_energy = energy * 0.7f + last_energy * 0.3f;

    // Base level from overall energy so it "breathes" a bit
    float base_level = get_focused_energy() * 0.15f;
    if (base_level < 0.03f && g_audio_energy > SILENCE_THRESHOLD) {
        base_level = 0.03f;
    }

    clear_leds();

    // Single color flash across whole strip using HSV rainbow
    float v = base_level + flash_intensity * 0.9f;
    if (v > 1.0f) v = 1.0f;
    uint8_t value = (uint8_t)(v * 255.0f);

    uint8_t r, g, b;
    hsv_to_rgb(base_hue, 255, value, &r, &g, &b);
    apply_brightness(&r, &g, &b, g_config.brightness);

    for (int i = 0; i < n; i++) {
        set_led(i, r, g, b);
    }
}

// Effect: Energy comet (bright core with long tail, trail length from energy)
static void effect_energy_comet(void)
{
    static int head = 0;

    int n = g_config.num_leds;
    if (n <= 0) return;

    float energy = get_focused_energy();
    if (energy < 0.05f) energy = 0.05f;

    clear_leds();

    int min_trail = n / 12;
    if (min_trail < 3) min_trail = 3;
    int max_trail = n / 3;
    if (max_trail < min_trail) max_trail = min_trail;

    int trail = min_trail + (int)((max_trail - min_trail) * energy);

    for (int i = 0; i < n; i++) {
        int dist = abs(i - head);
        if (dist > trail) continue;

        float t = (float)dist / (float)(trail > 0 ? trail : 1);
        float level = (1.0f - t * t) * energy; // quadratic falloff
        if (level <= 0.0f) continue;
        if (level > 1.0f) level = 1.0f;

        // Core towards secondary, tail towards primary
        float mix = t;
        uint8_t base_r = (uint8_t)((1.0f - mix) * g_config.color2.r + mix * g_config.color1.r);
        uint8_t base_g = (uint8_t)((1.0f - mix) * g_config.color2.g + mix * g_config.color1.g);
        uint8_t base_b = (uint8_t)((1.0f - mix) * g_config.color2.b + mix * g_config.color1.b);

        uint8_t r = (uint8_t)(base_r * level);
        uint8_t g = (uint8_t)(base_g * level);
        uint8_t b = (uint8_t)(base_b * level);

        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(i, r, g, b);
    }

    int step = 1 + (g_config.speed / 40);
    head += step;
    if (head >= n) head -= n;
}

// Effect: Fire / flicker (audio-driven flame)
static void effect_segment_levels(void)
{
    int n = g_config.num_leds;
    if (n <= 0) return;

    float bass = scale_by_sensitivity(g_bass_avg);
    if (bass < 0.0f) bass = 0.0f;
    if (bass > 1.0f) bass = 1.0f;

    clear_leds();

    for (int i = 0; i < n; i++) {
        // Treat index as "height" in the flame: hotter towards one end
        float pos = (float)i / (float)(n > 1 ? n - 1 : 1); // 0..1

        // Base intensity from bass + position so top glows more
        float base = bass * (0.3f + 0.7f * pos);

        // Random flicker per LED
        float rnd = (float)(esp_random() & 0xFF) / 255.0f; // 0..1
        float level = base * (0.6f + 0.4f * rnd);
        if (level > 1.0f) level = 1.0f;

        // Use a gradient between color1 (deeper) and color2 (hotter)
        uint8_t r = (uint8_t)((1.0f - pos) * g_config.color1.r + pos * g_config.color2.r);
        uint8_t g = (uint8_t)((1.0f - pos) * g_config.color1.g + pos * g_config.color2.g);
        uint8_t b = (uint8_t)((1.0f - pos) * g_config.color1.b + pos * g_config.color2.b);

        r = (uint8_t)(r * level);
        g = (uint8_t)(g * level);
        b = (uint8_t)(b * level);

        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(i, r, g, b);
    }
}

// Effect: Center ripples (beats launch waves from the middle)
static void effect_center_pulse(void)
{
    typedef struct {
        float pos;
        float radius;
        float strength;
        float width;
        int   active;
    } ripple_t;

    enum { MAX_RIPPLES = 3 };
    static ripple_t ripples[MAX_RIPPLES];
    static float last_energy = 0.0f;
    // Sub-bass tracking: fast envelope vs slow floor
    static float bass_env = 0.0f;
    static float bass_floor = 0.0f;
    static float last_env = 0.0f;
    static int   ripple_cooldown = 0;

    int n = g_config.num_leds;
    if (n <= 0) return;

    float energy = g_energy_avg;

    // Use the raw lowest band as "sub bass" input
    float raw_bass = g_bass_energy;
    if (raw_bass < 0.0f) raw_bass = 0.0f;
    if (raw_bass > 1.0f) raw_bass = 1.0f;

    // Fast attack / slower release envelope
    bass_env = 0.6f * raw_bass + 0.4f * bass_env;
    // Very slow-moving floor to capture average bass level
    bass_floor = 0.02f * raw_bass + 0.98f * bass_floor;

    // Short-term envelope change (edge detector)
    float delta_env = bass_env - last_env;

    float sens = g_config.sensitivity / 255.0f;
    // Thresholds for floor separation and fast edge
    float base_thresh_floor = 0.08f;                       // floor separation
    float thresh_floor = base_thresh_floor * (1.0f - sens * 0.7f);

    float base_thresh_edge = 0.030f;                       // fast edge
    float thresh_edge = base_thresh_edge * (1.0f - sens * 0.5f);

    bool floor_hit = (bass_env - bass_floor) > thresh_floor;
    bool edge_hit  = delta_env > thresh_edge;

    if (ripple_cooldown > 0) {
        ripple_cooldown--;
    }

    // Spawn a new ripple when sub-bass envelope makes a clear move
    if ((floor_hit || edge_hit) && raw_bass > 0.012f && ripple_cooldown == 0) {
        for (int i = 0; i < MAX_RIPPLES; i++) {
            if (!ripples[i].active) {
                ripples[i].active = 1;
                ripples[i].pos = (float)(n - 1) / 2.0f;
                ripples[i].radius = 0.0f;
                // Stronger sub-bass gives a slightly stronger ripple
                float bass_boost = 0.6f + raw_bass * 1.6f; // ~0.6 .. 2.2
                ripples[i].strength = bass_boost;
                ripples[i].width = 1.5f + (g_config.speed / 255.0f) * 2.5f;
                ripple_cooldown = 2; // small cooldown to avoid double-firing on the same hit
                break;
            }
        }
    }

    last_energy = energy * 0.7f + last_energy * 0.3f;
    last_env = bass_env;

    clear_leds();

    // Use sub-bass-weighted energy for base glow so it breathes with low end
    float base_mix = 0.3f * g_energy_avg + 0.9f * bass_env;
    float base_level = scale_by_sensitivity(base_mix) * 0.12f;
    if (base_level < 0.02f && g_audio_energy > SILENCE_THRESHOLD) {
        base_level = 0.02f;
    }

    float speed = 0.6f + (g_config.speed / 255.0f) * 1.2f;

    // Advance ripples
    for (int i = 0; i < MAX_RIPPLES; i++) {
        if (!ripples[i].active) continue;

        ripples[i].radius += speed;
        ripples[i].strength *= 0.90f;

        if (ripples[i].radius > (float)n || ripples[i].strength < 0.05f) {
            ripples[i].active = 0;
        }
    }

    for (int idx = 0; idx < n; idx++) {
        float pos = (float)idx;
        float ripple_sum = 0.0f;

        for (int r = 0; r < MAX_RIPPLES; r++) {
            if (!ripples[r].active) continue;

            float dist = fabsf(pos - ripples[r].pos);
            float diff = fabsf(dist - ripples[r].radius);
            if (diff > ripples[r].width) continue;

            float t = 1.0f - (diff / ripples[r].width);
            float v = ripples[r].strength * t;
            ripple_sum += v;
        }

        if (ripple_sum > 1.0f) ripple_sum = 1.0f;

        float level = base_level + ripple_sum;
        if (level > 1.0f) level = 1.0f;

        // Color gradient from center out using primary/secondary
        float x = (float)idx / (float)(n > 1 ? n - 1 : 1);
        uint8_t r = (uint8_t)((1.0f - x) * g_config.color1.r + x * g_config.color2.r);
        uint8_t g = (uint8_t)((1.0f - x) * g_config.color1.g + x * g_config.color2.g);
        uint8_t b = (uint8_t)((1.0f - x) * g_config.color1.b + x * g_config.color2.b);

        r = (uint8_t)(r * level);
        g = (uint8_t)(g * level);
        b = (uint8_t)(b * level);

        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(idx, r, g, b);
    }
}

// Effect: Scan multi (single bar scanning with alternating colors)
static void effect_scan_multi(void)
{
    static int pos = 0;
    static int dir = 1;

    int n = g_config.num_leds;
    if (n <= 0) return;

    float energy = scale_by_sensitivity(g_energy_avg);
    if (energy < 0.1f) energy = 0.1f;

    clear_leds();

    int width = 3 + (g_config.speed / 64);
    if (width < 2) width = 2;

    for (int i = -width / 2; i <= width / 2; i++) {
        int idx = pos + i;
        if (idx < 0 || idx >= n) continue;

        bool use_primary = ((pos / width) % 2) == 0;
        uint8_t r = use_primary ? g_config.color1.r : g_config.color2.r;
        uint8_t g = use_primary ? g_config.color1.g : g_config.color2.g;
        uint8_t b = use_primary ? g_config.color1.b : g_config.color2.b;

        float rel = 1.0f - (fabsf((float)i) / (float)(width / 2 + 1));
        float level = energy * rel;
        if (level > 1.0f) level = 1.0f;
        uint8_t intensity = (uint8_t)(level * 255.0f);

        r = (r * intensity) >> 8;
        g = (g * intensity) >> 8;
        b = (b * intensity) >> 8;

        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(idx, r, g, b);
    }

    int step = 1 + (g_config.speed / 48);
    pos += dir * step;
    if (pos >= n - 1) {
        pos = n - 1;
        dir = -1;
    } else if (pos <= 0) {
        pos = 0;
        dir = 1;
    }
}

// Effect: Pitch spectrum (map bands to hue along strip)
static void effect_pitch_spectrum(void)
{
    int n = g_config.num_leds;
    if (n <= 0) return;

    clear_leds();

    for (int i = 0; i < n; i++) {
        float x = (float)i / (float)(n - 1 > 0 ? n - 1 : 1); // 0..1
        float band_f = x * (float)(NUM_SPECTRUM_BANDS - 1);
        int band = (int)(band_f + 0.5f);
        if (band < 0) band = 0;
        if (band >= NUM_SPECTRUM_BANDS) band = NUM_SPECTRUM_BANDS - 1;

        float level = scale_by_sensitivity(g_band_avg[band]);
        if (level < 0.02f) continue;

        // Map band index to hue across spectrum
        uint16_t hue = (uint16_t)((band / (float)(NUM_SPECTRUM_BANDS - 1)) * 255.0f);
        uint8_t r, g, b;
        uint8_t value = (uint8_t)(level * 255.0f);
        hsv_to_rgb(hue, 255, value, &r, &g, &b);

        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(i, r, g, b);
    }
}

// Effect: Rainbow plasma (smooth, audio-reactive color field)
static void effect_rainbow_plasma(void)
{
    static float time_accum = 0.0f;

    int n = g_config.num_leds;
    if (n <= 0) return;

    float dt = 0.03f + (g_config.speed / 255.0f) * 0.12f; // 0.03..0.15
    time_accum += dt;

    float energy = get_focused_energy();
    if (energy < 0.0f) energy = 0.0f;
    if (energy > 1.0f) energy = 1.0f;

    clear_leds();

    for (int i = 0; i < n; i++) {
        float x = (float)i / (float)(n > 1 ? n - 1 : 1); // 0..1

        // 1D plasma: combine a couple of moving wave patterns
        float v1 = sinf(2.0f * (float)M_PI * (x * 1.3f + time_accum * 0.22f));
        float v2 = sinf(2.0f * (float)M_PI * (x * 2.7f - time_accum * 0.17f));
        float plasma = (v1 + v2) * 0.5f; // -1..1

        // Map plasma value to hue, modulated slightly by audio energy
        float hue_f = 0.5f + 0.3f * plasma + energy * 0.2f; // around middle of wheel
        if (hue_f < 0.0f) hue_f = 0.0f;
        if (hue_f > 1.0f) hue_f = 1.0f;
        uint16_t hue = (uint16_t)(hue_f * 255.0f) & 0xFF;

        // Brightness breathes with energy
        float base_v = 0.25f + 0.5f * energy;
        if (g_audio_energy < SILENCE_THRESHOLD) {
            base_v *= 0.4f; // dim when silent
        }

        // Extra local shimmer from band energies
        float band_idx_f = x * (float)(NUM_SPECTRUM_BANDS - 1);
        int band = (int)(band_idx_f + 0.5f);
        if (band < 0) band = 0;
        if (band >= NUM_SPECTRUM_BANDS) band = NUM_SPECTRUM_BANDS - 1;
        float band_level = g_band_avg[band];
        if (band_level > 1.0f) band_level = 1.0f;

        float v = base_v + band_level * 0.4f;
        if (v > 1.0f) v = 1.0f;

        uint8_t r, g, b;
        hsv_to_rgb(hue, 255, (uint8_t)(v * 255.0f), &r, &g, &b);

        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(i, r, g, b);
    }
}

// Simple 2D value-noise helper (Perlin-like)
static float noise2d(int x, int y)
{
    uint32_t h = (uint32_t)(x * 374761393 + y * 668265263); // large primes
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;
    // Map to -1..1
    return ((float)(h & 0xFFFF) / 32767.5f) - 1.0f;
}

static float smooth_noise(float x, float y)
{
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    float sx = x - (float)x0;
    float sy = y - (float)y0;

    float n00 = noise2d(x0, y0);
    float n10 = noise2d(x1, y0);
    float n01 = noise2d(x0, y1);
    float n11 = noise2d(x1, y1);

    // Cosine interpolation for smoother transitions
    float wx = 0.5f * (1.0f - cosf(sx * (float)M_PI));
    float wy = 0.5f * (1.0f - cosf(sy * (float)M_PI));

    float xa = n00 + wx * (n10 - n00);
    float xb = n01 + wx * (n11 - n01);
    float v = xa + wy * (xb - xa);

    return v; // -1..1
}

// Effect: Noise plasma (Perlin-style value noise field)
static void effect_noise_plasma(void)
{
    static float t = 0.0f;

    int n = g_config.num_leds;
    if (n <= 0) return;

    float speed = 0.01f + (g_config.speed / 255.0f) * 0.05f; // 0.01..0.06
    t += speed;

    float energy = get_focused_energy();
    if (energy < 0.0f) energy = 0.0f;
    if (energy > 1.0f) energy = 1.0f;

    clear_leds();

    for (int i = 0; i < n; i++) {
        float x = (float)i / (float)(n > 1 ? n - 1 : 1); // 0..1

        // Sample a couple of octaves of smooth value noise
        float nx = x * 4.0f;         // spatial scale
        float ny = t * 1.3f;         // time axis

        float n1 = smooth_noise(nx, ny);
        float n2 = smooth_noise(nx * 2.0f, ny * 2.0f) * 0.5f;
        float n3 = smooth_noise(nx * 4.0f, ny * 0.7f) * 0.25f;

        float noise_val = (n1 + n2 + n3) / (1.0f + 0.5f + 0.25f); // -1..1

        // Map noise to hue, let audio energy push hue towards hotter side
        float hue_f = 0.5f + 0.35f * noise_val + 0.15f * energy;
        if (hue_f < 0.0f) hue_f = 0.0f;
        if (hue_f > 1.0f) hue_f = 1.0f;
        uint16_t hue = (uint16_t)(hue_f * 255.0f) & 0xFF;

        // Brightness from energy plus local noise detail
        float v = 0.25f + 0.45f * energy + 0.2f * fabsf(noise_val);
        if (g_audio_energy < SILENCE_THRESHOLD) {
            v *= 0.4f; // dim when silent
        }
        if (v > 1.0f) v = 1.0f;

        uint8_t r, g, b;
        hsv_to_rgb(hue, 255, (uint8_t)(v * 255.0f), &r, &g, &b);

        apply_brightness(&r, &g, &b, g_config.brightness);
        set_led(i, r, g, b);
    }
}

// LED update task
static void led_task(void *param)
{
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t frame_delay = pdMS_TO_TICKS(33);  // ~30 FPS
    
    ESP_LOGI(TAG, "LED task started");
    
    while (1) {
        if (xSemaphoreTake(g_config_mutex, portMAX_DELAY) == pdTRUE) {
            led_effect_t effect = g_config.effect;

            // Update auto-rainbow primary color if enabled
            if (g_config.auto_rainbow) {
                uint8_t r, g, b;

                // Advance hue based on dedicated rainbow_speed setting
                uint8_t speed = g_config.rainbow_speed;
                uint8_t step = 1 + (speed / 12); // 1..22 steps per frame
                g_auto_rainbow_hue = (g_auto_rainbow_hue + step) & 0xFF;

                hsv_to_rgb(g_auto_rainbow_hue, 255, 255, &r, &g, &b);
                g_config.color1.r = r;
                g_config.color1.g = g;
                g_config.color1.b = b;
            }

            // Update auto-rainbow secondary color if enabled
            if (g_config.auto_rainbow2) {
                uint8_t r2, g2, b2;

                uint8_t speed2 = g_config.rainbow_speed2;
                uint8_t step2 = 1 + (speed2 / 12);
                g_auto_rainbow2_hue = (g_auto_rainbow2_hue + step2) & 0xFF;

                hsv_to_rgb(g_auto_rainbow2_hue, 255, 255, &r2, &g2, &b2);
                g_config.color2.r = r2;
                g_config.color2.g = g2;
                g_config.color2.b = b2;
            }

            xSemaphoreGive(g_config_mutex);
            
            // When audio has been silent for a short time, turn
            // off reactive effects instead of leaving them flickering.
            uint64_t now_us = esp_timer_get_time();
            bool no_recent_audio = (g_last_audio_time_us == 0) ||
                                   (now_us - g_last_audio_time_us > 500000); // > 500 ms
            bool silent = g_is_silent || no_recent_audio;
            if (silent && effect != LED_EFFECT_OFF && effect != LED_EFFECT_SOLID_COLOR) {
                clear_leds();
                update_leds();
            } else {
                bool do_update = true;
                switch (effect) {
                    case LED_EFFECT_OFF:
                        clear_leds();
                        break;
                    case LED_EFFECT_VU_METER:
                        effect_vu_meter();
                        break;
                    case LED_EFFECT_STEREO_VU:
                        effect_stereo_vu();
                        break;
                    case LED_EFFECT_SPECTRUM_ANALYZER:
                        effect_spectrum();
                        break;
                    case LED_EFFECT_PULSE:
                        effect_pulse();
                        break;
                    case LED_EFFECT_WAVE:
                        effect_wave();
                        break;
                    case LED_EFFECT_ENERGY_BAR:
                        effect_energy_bar();
                        break;
                    case LED_EFFECT_RAINBOW_PULSE:
                        effect_rainbow_pulse();
                        break;
                    case LED_EFFECT_BEAT_FLASH:
                        effect_beat_flash();
                        break;
                    case LED_EFFECT_BASS_PULSE:
                        effect_bass_pulse();
                        break;
                    case LED_EFFECT_SOLID_COLOR:
                    {
                        // Only resend solid color if brightness or color changed
                        led_color_t current_color;
                        uint8_t current_brightness;

                        if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                            current_color = g_config.color1;
                            current_brightness = g_config.brightness;
                            xSemaphoreGive(g_config_mutex);
                        } else {
                            // If we can't get the config, just draw as usual
                            current_color = g_config.color1;
                            current_brightness = g_config.brightness;
                        }

                        bool changed = !g_solid_cached ||
                                       current_brightness != g_solid_brightness ||
                                       current_color.r != g_solid_color.r ||
                                       current_color.g != g_solid_color.g ||
                                       current_color.b != g_solid_color.b;

                        if (changed) {
                            g_solid_cached = true;
                            g_solid_color = current_color;
                            g_solid_brightness = current_brightness;
                            effect_solid();
                        } else {
                            // Nothing changed, no need to resend frame
                            do_update = false;
                        }
                        break;
                    }
                        break;
                    case LED_EFFECT_COLOR_WIPE:
                        effect_color_wipe();
                        break;
                    case LED_EFFECT_DUAL_COLOR_WAVE:
                        effect_dual_color_wave();
                        break;
                    case LED_EFFECT_COMET:
                        effect_comet();
                        break;
                    case LED_EFFECT_SPARKLE:
                        effect_sparkle();
                        break;
                    case LED_EFFECT_BEAT_RAINBOW:
                        effect_beat_rainbow();
                        break;
                    case LED_EFFECT_MULTI_COMET:
                        effect_multi_comet();
                        break;
                    case LED_EFFECT_MARCHING:
                        effect_marching();
                        break;
                    case LED_EFFECT_BLADE_POWER:
                        effect_blade_power();
                        break;
                    case LED_EFFECT_SCAN_MULTI:
                        effect_scan_multi();
                        break;
                    case LED_EFFECT_PITCH_SPECTRUM:
                        effect_pitch_spectrum();
                        break;
                    case LED_EFFECT_EQUALIZER:
                        effect_equalizer();
                        break;
                    case LED_EFFECT_DUAL_ENERGY_BAR:
                        effect_dual_energy_bar();
                        break;
                    case LED_EFFECT_CHARGE_BEAM:
                        effect_charge_beam();
                        break;
                    case LED_EFFECT_ENERGY_COMET:
                        effect_energy_comet();
                        break;
                    case LED_EFFECT_SEGMENT_LEVELS:
                        effect_segment_levels();
                        break;
                    case LED_EFFECT_CENTER_PULSE:
                        effect_center_pulse();
                        break;
                    case LED_EFFECT_NOISE_PLASMA:
                        effect_noise_plasma();
                        break;
                    case LED_EFFECT_RAINBOW_PLASMA:
                        effect_rainbow_plasma();
                        break;
                    default:
                        clear_leds();
                        break;
                }
                if (do_update) {
                    update_leds();
                }
            }
        }
        
        vTaskDelayUntil(&last_wake, frame_delay);
    }
}

// Public API implementation

esp_err_t led_controller_init(led_config_t *config)
{
    if (!config || config->num_leds == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Initializing LED controller: pin=%d, num_leds=%d", 
             config->gpio_pin, config->num_leds);
    
    // Copy config
    memcpy(&g_config, config, sizeof(led_config_t));
    
    // Create mutex
    g_config_mutex = xSemaphoreCreateMutex();
    if (!g_config_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    
    // Allocate LED buffer (RGB * num_leds)
    g_led_buffer = calloc(g_config.num_leds, 3);
    if (!g_led_buffer) {
        ESP_LOGE(TAG, "Failed to allocate LED buffer");
        vSemaphoreDelete(g_config_mutex);
        return ESP_ERR_NO_MEM;
    }
    
    // Configure RMT TX channel
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = g_config.gpio_pin,
        .mem_block_symbols = 64,
        .resolution_hz = 10 * 1000 * 1000,  // 10MHz, 100ns resolution
        .trans_queue_depth = 4,
        .flags.invert_out = false,
        .flags.with_dma = false,
    };
    
    esp_err_t ret = rmt_new_tx_channel(&tx_chan_config, &g_led_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT TX channel: %s", esp_err_to_name(ret));
        free(g_led_buffer);
        vSemaphoreDelete(g_config_mutex);
        return ret;
    }
    
    // Create LED strip encoder
    ret = rmt_new_led_strip_encoder(&g_led_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED encoder");
        rmt_del_channel(g_led_chan);
        free(g_led_buffer);
        vSemaphoreDelete(g_config_mutex);
        return ret;
    }
    
    // Enable RMT channel
    ret = rmt_enable(g_led_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RMT channel");
        rmt_del_encoder(g_led_encoder);
        rmt_del_channel(g_led_chan);
        free(g_led_buffer);
        vSemaphoreDelete(g_config_mutex);
        return ret;
    }
    
    // Clear LEDs initially
    clear_leds();
    update_leds();
    
    // Create LED update task
    BaseType_t task_ret = xTaskCreate(led_task, "led_task", 4096, NULL, 5, &g_led_task);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LED task");
        rmt_disable(g_led_chan);
        rmt_del_encoder(g_led_encoder);
        rmt_del_channel(g_led_chan);
        free(g_led_buffer);
        vSemaphoreDelete(g_config_mutex);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "LED controller initialized successfully");
    return ESP_OK;
}

esp_err_t led_controller_deinit(void)
{
    if (g_led_task) {
        vTaskDelete(g_led_task);
        g_led_task = NULL;
    }
    
    if (g_led_chan) {
        rmt_disable(g_led_chan);
        rmt_del_channel(g_led_chan);
        g_led_chan = NULL;
    }
    
    if (g_led_encoder) {
        rmt_del_encoder(g_led_encoder);
        g_led_encoder = NULL;
    }
    
    if (g_led_buffer) {
        free(g_led_buffer);
        g_led_buffer = NULL;
    }
    
    if (g_config_mutex) {
        vSemaphoreDelete(g_config_mutex);
        g_config_mutex = NULL;
    }
    
    ESP_LOGI(TAG, "LED controller deinitialized");
    return ESP_OK;
}

esp_err_t led_controller_set_effect(led_effect_t effect)
{
    if (effect >= LED_EFFECT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_config_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_config.effect = effect;
        xSemaphoreGive(g_config_mutex);
        ESP_LOGI(TAG, "Effect set to %d", effect);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t led_controller_set_brightness(uint8_t brightness)
{
    if (!g_config_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_config.brightness = brightness;
        xSemaphoreGive(g_config_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t led_controller_set_color(led_color_t color1, led_color_t color2)
{
    if (!g_config_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_config.color1 = color1;
        g_config.color2 = color2;
        xSemaphoreGive(g_config_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t led_controller_set_speed(uint8_t speed)
{
    if (!g_config_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_config.speed = speed;
        xSemaphoreGive(g_config_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t led_controller_set_sensitivity(uint8_t sensitivity)
{
    if (!g_config_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_config.sensitivity = sensitivity;
        xSemaphoreGive(g_config_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t led_controller_set_bass_focus(uint8_t bass_focus)
{
    if (!g_config_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_config.bass_focus = bass_focus;
        xSemaphoreGive(g_config_mutex);
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t led_controller_set_rainbow_speed(uint8_t speed)
{
    if (!g_config_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_config.rainbow_speed = speed;
        xSemaphoreGive(g_config_mutex);
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t led_controller_set_auto_rainbow(uint8_t enabled)
{
    if (!g_config_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_config.auto_rainbow = enabled ? 1 : 0;
        xSemaphoreGive(g_config_mutex);
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t led_controller_set_rainbow_speed2(uint8_t speed)
{
    if (!g_config_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_config.rainbow_speed2 = speed;
        xSemaphoreGive(g_config_mutex);
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t led_controller_set_auto_rainbow2(uint8_t enabled)
{
    if (!g_config_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_config.auto_rainbow2 = enabled ? 1 : 0;
        xSemaphoreGive(g_config_mutex);
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t led_controller_get_config(led_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_config_mutex) {
        // Controller not initialized; signal this to caller.
        memset(config, 0, sizeof(led_config_t));
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(config, &g_config, sizeof(led_config_t));
        xSemaphoreGive(g_config_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t led_controller_feed_audio(const int16_t *audio_data, size_t num_samples)
{
    if (!audio_data || num_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Mark time of last received audio frame (used to detect pause/mute)
    g_last_audio_time_us = esp_timer_get_time();

    // Simple energy calculation (RMS)
    float energy = 0;
    
    // Calculate overall energy
    for (size_t i = 0; i < num_samples * 2; i += 2) {
        float sample = (audio_data[i] + audio_data[i + 1]) / 2.0f / 32768.0f;
        energy += sample * sample;
    }
    energy = sqrtf(energy / num_samples);

    // Sub-bass estimate using a compact spectral bin
    float sub_bass = compute_sub_bass_energy(audio_data, num_samples);

    // Silence detection: if RMS stays very low for several frames,
    // treat this as muted/paused audio so LEDs for reactive effects
    // can be turned off.
    if (energy < SILENCE_THRESHOLD) {
        if (g_silence_frames < UINT32_MAX) {
            g_silence_frames++;
        }
    } else {
        g_silence_frames = 0;
    }
    g_is_silent = (g_silence_frames >= SILENCE_HOLD_FRAMES);
    
    // Simple multi-band estimation (primitive but lightweight)
    // Split the time-domain samples into NUM_SPECTRUM_BANDS equal segments
    for (int band = 0; band < NUM_SPECTRUM_BANDS; band++) {
        float band_energy = 0;
        size_t start_frame = (num_samples * band) / NUM_SPECTRUM_BANDS;
        size_t end_frame   = (num_samples * (band + 1)) / NUM_SPECTRUM_BANDS;
        size_t samples_in_band = 0;

        for (size_t frame = start_frame; frame < end_frame; frame++) {
            size_t idx = frame * 2;
            if (idx + 1 >= num_samples * 2) {
                break;
            }
            float sample = (audio_data[idx] + audio_data[idx + 1]) / 2.0f / 32768.0f;
            band_energy += sample * sample;
            samples_in_band++;
        }

        if (samples_in_band > 0) {
            band_energy = sqrtf(band_energy / samples_in_band);
        } else {
            band_energy = 0.0f;
        }

        g_band_energy[band] = band_energy;
    }

    // Note: ignore_volume is handled at the integration points
    // (whether we feed pre- or post-volume audio into this
    // function). The energy calculation here always reflects
    // whatever PCM data is provided.

    // Clamp to a sane range before smoothing
    if (energy > 1.0f) energy = 1.0f;
    if (energy < 0.0f) energy = 0.0f;
    for (int band = 0; band < NUM_SPECTRUM_BANDS; band++) {
        if (g_band_energy[band] > 1.0f) g_band_energy[band] = 1.0f;
        if (g_band_energy[band] < 0.0f) g_band_energy[band] = 0.0f;
    }

    // Smooth with moving average
    const float alpha = 0.3f;
    g_energy_avg = alpha * energy + (1.0f - alpha) * g_energy_avg;
    for (int band = 0; band < NUM_SPECTRUM_BANDS; band++) {
        g_band_avg[band] = alpha * g_band_energy[band] + (1.0f - alpha) * g_band_avg[band];
    }

    // Backwards-compatible aggregates
    // Use spectral sub-bass estimate for bass metrics so effects that
    // key off bass (Center Ripples, Bass Pulse, etc.) see true low end.
    const float bass_alpha = 0.3f;
    g_bass_avg = bass_alpha * sub_bass + (1.0f - bass_alpha) * g_bass_avg;
    g_mid_energy   = (g_band_avg[2] + g_band_avg[3]) * 0.5f;
    g_treble_energy = (g_band_avg[4] + g_band_avg[5]) * 0.5f;

    g_audio_energy = energy;
    g_bass_energy  = sub_bass;
    g_frame_count++;
    
    return ESP_OK;
}

// Lightweight helper for sources that only expose a volume level, such as
// Bluetooth A2DP absolute volume. "level" should be in the range 0.0 - 1.0
// after all gain/volume has been applied. This function updates the internal
// energy and silence detection so effects can react without raw PCM samples.
esp_err_t led_controller_feed_level(float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    // Treat level as RMS energy
    float energy = level;

    // Mark audio as recently active
    g_last_audio_time_us = esp_timer_get_time();

    // Silence detection: use same logic as PCM path
    if (energy < SILENCE_THRESHOLD) {
        if (g_silence_frames < UINT32_MAX) {
            g_silence_frames++;
        }
    } else {
        g_silence_frames = 0;
    }
    g_is_silent = (g_silence_frames >= SILENCE_HOLD_FRAMES);

    // Simple smoothing on overall energy only
    const float alpha = 0.3f;
    g_energy_avg = alpha * energy + (1.0f - alpha) * g_energy_avg;
    g_audio_energy = energy;

    return ESP_OK;
}
