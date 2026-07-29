#include "display_sh1106.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"

static const char *TAG = "DISPLAY";

// SH1106 Commands
#define SH1106_SETCONTRAST 0x81
#define SH1106_DISPLAYALLON_RESUME 0xA4
#define SH1106_DISPLAYALLON 0xA5
#define SH1106_NORMALDISPLAY 0xA6
#define SH1106_INVERTDISPLAY 0xA7
#define SH1106_DISPLAYOFF 0xAE
#define SH1106_DISPLAYON 0xAF
#define SH1106_SETDISPLAYOFFSET 0xD3
#define SH1106_SETCOMPINS 0xDA
#define SH1106_SETVCOMDETECT 0xDB
#define SH1106_SETDISPLAYCLOCKDIV 0xD5
#define SH1106_SETPRECHARGE 0xD9
#define SH1106_SETMULTIPLEX 0xA8
#define SH1106_SETLOWCOLUMN 0x00
#define SH1106_SETHIGHCOLUMN 0x10
#define SH1106_SETSTARTLINE 0x40
#define SH1106_MEMORYMODE 0x20
#define SH1106_COLUMNADDR 0x21
#define SH1106_PAGEADDR 0x22
#define SH1106_COMSCANINC 0xC0
#define SH1106_COMSCANDEC 0xC8
#define SH1106_SEGREMAP 0xA0
#define SH1106_CHARGEPUMP 0x8D
#define SH1106_EXTERNALVCC 0x1
#define SH1106_SWITCHCAPVCC 0x2
#define SH1106_SETPAGEADDR 0xB0

// Display buffer and state
static uint8_t display_buffer[SH1106_WIDTH * SH1106_HEIGHT / 8];
static song_metadata_t current_metadata = {0};
static audio_viz_t current_audio = {0};
static wifi_signal_t current_wifi = {.rssi = -100, .signal_bars = 0, .wifi_connected = false};
static bt_signal_t current_bt = {.rssi = -100, .signal_bars = 0, .bt_connected = false};
static SemaphoreHandle_t display_mutex = NULL;
static TaskHandle_t display_task_handle = NULL;
static bool display_initialized = false;
static char current_device_name[32] = "BT"; // Default fallback
static char bt_connected_device_name[64] = ""; // Connected BT device name
static uint8_t snapcast_volume_level = 50; // Track snapcast volume separately from BT
static bool snapcast_muted = false; // Track snapcast mute state

// Scrolling text state
static int title_scroll_pos = 0;
static int artist_scroll_pos = 0;
static int device_name_scroll_pos = 0;  // New scrolling for device name
static uint32_t last_scroll_time = 0;
// Scroll a bit slower to reduce I2C bus usage
static const int SCROLL_SPEED_MS = 400;  // Scroll every 400ms
static const int MAX_DISPLAY_CHARS = 20;  // Characters that fit on screen

// Runtime column offset, defaults to compile-time value but can be
// overridden from system_config via sh1106_column_offset
static int s_display_column_offset = SH1106_COLUMN_OFFSET;

void display_set_column_offset(int offset) {
    if (offset < 0) offset = 0;
    if (offset > 127) offset = 127;
    s_display_column_offset = offset;
}

// Simple 5x7 font for basic text
static const uint8_t font_5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // Space
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // !
    {0x00, 0x07, 0x00, 0x07, 0x00}, // "
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // $
    {0x23, 0x13, 0x08, 0x64, 0x62}, // %
    {0x36, 0x49, 0x55, 0x22, 0x50}, // &
    {0x00, 0x05, 0x03, 0x00, 0x00}, // '
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // (
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // )
    {0x08, 0x2A, 0x1C, 0x2A, 0x08}, // *
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // +
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ,
    {0x08, 0x08, 0x08, 0x08, 0x08}, // -
    {0x00, 0x60, 0x60, 0x00, 0x00}, // .
    {0x20, 0x10, 0x08, 0x04, 0x02}, // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x00, 0x36, 0x36, 0x00, 0x00}, // :
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ;
    {0x00, 0x08, 0x14, 0x22, 0x41}, // <
    {0x14, 0x14, 0x14, 0x14, 0x14}, // =
    {0x41, 0x22, 0x14, 0x08, 0x00}, // >
    {0x02, 0x01, 0x51, 0x09, 0x06}, // ?
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // @
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x09, 0x09, 0x01, 0x01}, // F
    {0x3E, 0x41, 0x41, 0x51, 0x32}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7F, 0x02, 0x04, 0x02, 0x7F}, // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x03, 0x04, 0x78, 0x04, 0x03}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
};

// I2C write command
static esp_err_t sh1106_write_command(uint8_t cmd) {
    uint8_t data[2] = {0x00, cmd}; // Co=0, D/C=0 (command)
    i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
    i2c_master_start(cmd_handle);
    i2c_master_write_byte(cmd_handle, (SH1106_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd_handle, data, 2, true);
    i2c_master_stop(cmd_handle);
    esp_err_t ret = i2c_master_cmd_begin(SH1106_I2C_PORT, cmd_handle, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd_handle);
    return ret;
}

// I2C write data
static esp_err_t sh1106_write_data(uint8_t* data, size_t len) {
    uint8_t control_byte = 0x40; // Co=0, D/C=1 (data)
    i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
    i2c_master_start(cmd_handle);
    i2c_master_write_byte(cmd_handle, (SH1106_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd_handle, control_byte, true);
    i2c_master_write(cmd_handle, data, len, true);
    i2c_master_stop(cmd_handle);
    esp_err_t ret = i2c_master_cmd_begin(SH1106_I2C_PORT, cmd_handle, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd_handle);
    return ret;
}

// Set pixel in buffer
static void set_pixel(int x, int y, bool on) {
    if (x >= 0 && x < SH1106_WIDTH && y >= 0 && y < SH1106_HEIGHT) {
        int page = y / 8;
        int bit = y % 8;
        int index = page * SH1106_WIDTH + x;
        
        if (on) {
            display_buffer[index] |= (1 << bit);
        } else {
            display_buffer[index] &= ~(1 << bit);
        }
    }
}

// Draw character (normal size)
static int draw_char(int x, int y, char c) {
    // Convert lowercase to uppercase since font only has uppercase
    if (c >= 'a' && c <= 'z') {
        c = c - 'a' + 'A';
    }
    
    // Handle unsupported chars - font covers space (32) to Z (90)
    if (c < ' ' || c > 'Z') c = ' ';
    int char_index = c - ' ';
    
    // Bounds check for font array (space to Z = 59 characters)
    if (char_index < 0 || char_index >= 59) {
        char_index = 0; // Default to space
    }
    
    for (int col = 0; col < 5; col++) {
        uint8_t font_col = font_5x7[char_index][col];
        for (int row = 0; row < 7; row++) {
            if (font_col & (1 << row)) {
                set_pixel(x + col, y + row, true);
            }
        }
    }
    return x + 6; // Character width + spacing
}

// Draw character 3x larger (for idle screen device name)
static int draw_char_3x(int x, int y, char c) {
    // Convert lowercase to uppercase since font only has uppercase
    if (c >= 'a' && c <= 'z') {
        c = c - 'a' + 'A';
    }
    
    // Handle unsupported chars - font covers space (32) to Z (90)
    if (c < ' ' || c > 'Z') c = ' ';
    int char_index = c - ' ';
    
    // Bounds check for font array (space to Z = 59 characters)
    if (char_index < 0 || char_index >= 59) {
        char_index = 0; // Default to space
    }
    
    for (int col = 0; col < 5; col++) {
        uint8_t font_col = font_5x7[char_index][col];
        for (int row = 0; row < 7; row++) {
            if (font_col & (1 << row)) {
                // Draw 3x3 block for each original pixel
                for (int sx = 0; sx < 3; sx++) {
                    for (int sy = 0; sy < 3; sy++) {
                        int px = x + (col * 3) + sx;
                        int py = y + (row * 3) + sy;
                        if (px < SH1106_WIDTH && py < SH1106_HEIGHT) {
                            set_pixel(px, py, true);
                        }
                    }
                }
            }
        }
    }
    return x + 18; // Character width (5*3) + spacing (3*1)
}

// Draw character 3x larger with bold effect (for clock)
static int draw_char_3x_bold(int x, int y, char c) {
    // Convert lowercase to uppercase since font only has uppercase
    if (c >= 'a' && c <= 'z') {
        c = c - 'a' + 'A';
    }
    
    // Handle unsupported chars - font covers space (32) to Z (90)
    if (c < ' ' || c > 'Z') c = ' ';
    int char_index = c - ' ';
    
    // Bounds check for font array (space to Z = 59 characters)
    if (char_index < 0 || char_index >= 59) {
        char_index = 0; // Default to space
    }
    
    for (int col = 0; col < 5; col++) {
        uint8_t font_col = font_5x7[char_index][col];
        for (int row = 0; row < 7; row++) {
            if (font_col & (1 << row)) {
                // Draw 3x3 block for each original pixel with bold effect
                for (int sx = 0; sx < 3; sx++) {
                    for (int sy = 0; sy < 3; sy++) {
                        int px = x + (col * 3) + sx;
                        int py = y + (row * 3) + sy;
                        if (px < SH1106_WIDTH && py < SH1106_HEIGHT) {
                            set_pixel(px, py, true);
                        }
                        // Add bold effect by drawing additional pixels
                        if (px + 1 < SH1106_WIDTH && py < SH1106_HEIGHT) {
                            set_pixel(px + 1, py, true);
                        }
                    }
                }
            }
        }
    }
    
    return x + 19; // 3x character width + bold offset + spacing
}

// Draw string (normal size)
static int draw_string(int x, int y, const char* str) {
    int pos_x = x;
    while (*str && pos_x < SH1106_WIDTH) {
        pos_x = draw_char(pos_x, y, *str++);
    }
    return pos_x;
}

// Draw character 1.5x larger (for device name)
static int draw_char_1_5x(int x, int y, char c) {
    // Convert lowercase to uppercase since font only has uppercase
    if (c >= 'a' && c <= 'z') {
        c = c - 'a' + 'A';
    }
    
    // Handle unsupported chars - font covers space (32) to Z (90)
    if (c < ' ' || c > 'Z') c = ' ';
    int char_index = c - ' ';
    
    // Bounds check for font array (space to Z = 59 characters)
    if (char_index < 0 || char_index >= 59) {
        char_index = 0; // Default to space
    }
    
    for (int col = 0; col < 5; col++) {
        uint8_t font_col = font_5x7[char_index][col];
        for (int row = 0; row < 7; row++) {
            if (font_col & (1 << row)) {
                // Draw 1.5x by drawing original pixel + half-size extensions
                set_pixel(x + col + (col / 2), y + row + (row / 2), true); // Main pixel with 1.5x spacing
                
                // Add thickness for better visibility at 1.5x
                if (col < 4) { // Don't extend past character boundary
                    set_pixel(x + col + (col / 2) + 1, y + row + (row / 2), true); // Right extension
                }
                if (row < 6) { // Don't extend past character boundary
                    set_pixel(x + col + (col / 2), y + row + (row / 2) + 1, true); // Bottom extension
                }
            }
        }
    }
    return x + 9; // Character width (~7.5 rounded to 8) + spacing (1)
}

// Draw character 2x larger (for slightly smaller device name)
static int draw_char_2x(int x, int y, char c) {
    // Convert lowercase to uppercase since font only has uppercase
    if (c >= 'a' && c <= 'z') {
        c = c - 'a' + 'A';
    }
    
    // Handle unsupported chars - font covers space (32) to Z (90)
    if (c < ' ' || c > 'Z') c = ' ';
    int char_index = c - ' ';
    
    // Bounds check for font array (space to Z = 59 characters)
    if (char_index < 0 || char_index >= 59) {
        char_index = 0; // Default to space
    }
    
    for (int col = 0; col < 5; col++) {
        uint8_t font_col = font_5x7[char_index][col];
        for (int row = 0; row < 7; row++) {
            if (font_col & (1 << row)) {
                // Draw 2x2 block for each original pixel
                for (int sx = 0; sx < 2; sx++) {
                    for (int sy = 0; sy < 2; sy++) {
                        int px = x + (col * 2) + sx;
                        int py = y + (row * 2) + sy;
                        if (px < SH1106_WIDTH && py < SH1106_HEIGHT) {
                            set_pixel(px, py, true);
                        }
                    }
                }
            }
        }
    }
    return x + 12; // Character width (5*2) + spacing (2*1)
}

// Draw string 1.5x larger (for device name)
static int draw_string_1_5x(int x, int y, const char* str) {
    int pos_x = x;
    while (*str && pos_x < SH1106_WIDTH) {
        pos_x = draw_char_1_5x(pos_x, y, *str++);
    }
    return pos_x;
}

// Draw string 2x larger (for slightly smaller device name)
static int draw_string_2x(int x, int y, const char* str) {
    int pos_x = x;
    while (*str && pos_x < SH1106_WIDTH) {
        pos_x = draw_char_2x(pos_x, y, *str++);
    }
    return pos_x;
}

// Draw string 3x larger (for idle screen device name)
static int draw_string_3x(int x, int y, const char* str) {
    int pos_x = x;
    while (*str && pos_x < SH1106_WIDTH) {
        pos_x = draw_char_3x(pos_x, y, *str++);
    }
    return pos_x;
}

// Draw string 3x larger with bold effect (for clock)
static int draw_string_3x_bold(int x, int y, const char* str) {
    int pos_x = x;
    while (*str && pos_x < SH1106_WIDTH) {
        pos_x = draw_char_3x_bold(pos_x, y, *str++);
    }
    return pos_x;
}

// Draw scrolling text
static void draw_scrolling_text(int x, int y, const char* str, int* scroll_pos) {
    if (!str || strlen(str) == 0) return;
    
    int str_len = strlen(str);
    
    // If text fits on screen, just display it normally
    if (str_len <= MAX_DISPLAY_CHARS) {
        draw_string(x, y, str);
        *scroll_pos = 0;
        return;
    }
    
    // Create scrolling effect
    char display_text[MAX_DISPLAY_CHARS + 4]; // +4 for spacing and null terminator
    memset(display_text, 0, sizeof(display_text));
    
    // Add some spaces for smooth cycling (limit to prevent buffer issues)
    char extended_str[256];
    int max_copy = (str_len < 252) ? str_len : 252; // Leave room for "   " and null
    int i;
    for (i = 0; i < max_copy && str[i] != '\0'; i++) {
        extended_str[i] = str[i];
    }
    extended_str[i] = '\0';
    // Add spacing only if room
    if (i < 253) extended_str[i++] = ' ';
    if (i < 254) extended_str[i++] = ' ';
    if (i < 255) extended_str[i++] = ' ';
    extended_str[i] = '\0';
    int extended_len = i;
    
    // Calculate visible portion
    for (int i = 0; i < MAX_DISPLAY_CHARS && i < extended_len; i++) {
        int char_idx = (*scroll_pos + i) % extended_len;
        display_text[i] = extended_str[char_idx];
    }
    
    draw_string(x, y, display_text);
}

// Draw scrolling text 1.5x larger (for device name)
static void draw_scrolling_text_1_5x(int x, int y, const char* str, int* scroll_pos) {
    if (!str || strlen(str) == 0) return;
    
    int str_len = strlen(str);
    const int MAX_1_5X_CHARS = 9; // Approximate chars that fit at 1.5x size (88px / 9px per char)
    
    // If text fits on screen at 1.5x size, just display it normally
    if (str_len <= MAX_1_5X_CHARS) {
        draw_string_1_5x(x, y, str);
        *scroll_pos = 0;
        return;
    }
    
    // Create scrolling effect for 1.5x text
    char display_text[MAX_1_5X_CHARS + 2]; // +1 for spacing and null terminator
    memset(display_text, 0, sizeof(display_text));
    
    // Add some spaces for smooth cycling
    char extended_str[256];
    snprintf(extended_str, sizeof(extended_str), "%s   ", str);
    int extended_len = strlen(extended_str);
    
    // Calculate visible portion
    for (int i = 0; i < MAX_1_5X_CHARS && i < extended_len; i++) {
        int char_idx = (*scroll_pos + i) % extended_len;
        display_text[i] = extended_str[char_idx];
    }
    
    draw_string_1_5x(x, y, display_text);
}

// Draw scrolling text 2x larger (for slightly smaller device name)
static void draw_scrolling_text_2x(int x, int y, const char* str, int* scroll_pos) {
    if (!str || strlen(str) == 0) return;
    
    int str_len = strlen(str);
    const int MAX_2X_CHARS = 10; // Approximate chars that fit at 2x size (128px / 12px per char)
    
    // If text fits on screen at 2x size, just display it normally
    if (str_len <= MAX_2X_CHARS) {
        draw_string_2x(x, y, str);
        *scroll_pos = 0;
        return;
    }
    
    // Create scrolling effect for 2x text
    char display_text[MAX_2X_CHARS + 2]; // +1 for spacing and null terminator
    memset(display_text, 0, sizeof(display_text));
    
    // Add some spaces for smooth cycling
    char extended_str[256];
    snprintf(extended_str, sizeof(extended_str), "%s   ", str);
    int extended_len = strlen(extended_str);
    
    // Calculate visible portion
    for (int i = 0; i < MAX_2X_CHARS && i < extended_len; i++) {
        int char_idx = (*scroll_pos + i) % extended_len;
        display_text[i] = extended_str[char_idx];
    }
    
    draw_string_2x(x, y, display_text);
}

// Draw scrolling text 3x larger (for device name)
static void draw_scrolling_text_3x(int x, int y, const char* str, int* scroll_pos) {
    if (!str || strlen(str) == 0) return;
    
    int str_len = strlen(str);
    const int MAX_3X_CHARS = 7; // Approximate chars that fit at 3x size (128px / 18px per char)
    
    // If text fits on screen at 3x size, just display it normally
    if (str_len <= MAX_3X_CHARS) {
        draw_string_3x(x, y, str);
        *scroll_pos = 0;
        return;
    }
    
    // Create scrolling effect for 3x text
    char display_text[MAX_3X_CHARS + 2]; // +1 for spacing and null terminator
    memset(display_text, 0, sizeof(display_text));
    
    // Add some spaces for smooth cycling
    char extended_str[256];
    snprintf(extended_str, sizeof(extended_str), "%s   ", str);
    int extended_len = strlen(extended_str);
    
    // Calculate visible portion
    for (int i = 0; i < MAX_3X_CHARS && i < extended_len; i++) {
        int char_idx = (*scroll_pos + i) % extended_len;
        display_text[i] = extended_str[char_idx];
    }
    
    draw_string_3x(x, y, display_text);
}

// Draw filled rectangle
static void draw_filled_rect(int x, int y, int width, int height) {
    for (int i = 0; i < width; i++) {
        for (int j = 0; j < height; j++) {
            set_pixel(x + i, y + j, true);
        }
    }
}

// Draw vertical volume bar or MUTED text
static void draw_volume_bar_or_muted(int x, int start_y, int end_y, uint8_t volume, bool muted) {
    if (muted) {
        // Draw "MUTED" text vertically
        const char* muted_text = "MUTE";
        int text_len = strlen(muted_text);
        int char_height = 11; // Font height
        int char_spacing = 1; // Spacing between characters
        int total_text_height = (text_len * char_height) + ((text_len - 1) * char_spacing);
        
        // Center the text vertically in the available space
        int available_height = end_y - start_y;
        int text_start_y = start_y + (available_height - total_text_height) / 2;
        
        // Draw each character vertically with extra width (2 pixels wider)
        for (int i = 0; i < text_len; i++) {
            int char_y = text_start_y + (i * (char_height + char_spacing));
            // Draw original character
            draw_char(x, char_y, muted_text[i]);
            // Draw additional pixels to make it 2 pixels wider
            draw_char(x + 1, char_y, muted_text[i]);
        }
    } else {
        // Draw normal volume bar
        int total_height = end_y - start_y;
        int filled_height = (volume * total_height) / 100;
        int fill_start_y = end_y - filled_height;
        
        // Draw solid filled portion (5 pixels wide, no border)
        for (int y = fill_start_y; y < end_y; y++) {
            set_pixel(x, y, true);
            set_pixel(x + 1, y, true);
            set_pixel(x + 2, y, true);
            set_pixel(x + 3, y, true);
            set_pixel(x + 4, y, true);
        }
    }
}

// Draw EQ bar
static void draw_eq_bar(int x, int height) {
    // Start EQ bars right after artist line (Y=18 + 7 pixels for font height = Y=25)
    // Now that line 4 is free, we can use more space: Y=27 to bottom (Y=64) = 37 pixels available
    int eq_start_y = 27; // Start where the old volume line was
    int available_height = SH1106_HEIGHT - eq_start_y; // 37 pixels available (64 - 27)
    int bar_height = (height * available_height) / 100;
    int y_start = SH1106_HEIGHT - bar_height;
    draw_filled_rect(x, y_start, EQ_BAR_WIDTH, bar_height);
}

// Clear display buffer
esp_err_t display_clear(void) {
    memset(display_buffer, 0, sizeof(display_buffer));
    return ESP_OK;
}

// Initialize SNTP for time synchronization (only when network is available)
static bool sntp_initialized = false;
static uint32_t sntp_init_time = 0;
static void initialize_sntp(void) {
    if (sntp_initialized) {
        return; // Already initialized
    }
    
    // Check if we have network connectivity by checking WiFi status
    if (!current_wifi.wifi_connected) {
        return; // Wait for WiFi connection
    }
    
    ESP_LOGI(TAG, "Initializing SNTP with WiFi connected");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_init();

    // Set timezone to Brisbane, Australia (UTC+10, no DST)
    setenv("TZ", "AEST-10", 1);
    tzset();
    
    sntp_initialized = true;
    sntp_init_time = xTaskGetTickCount();
    ESP_LOGI(TAG, "SNTP initialized successfully, waiting for time sync");
}

// Get current time in 12-hour format with seconds
static void get_current_time_12h(char* time_str, char* seconds_str, char* ampm_str, size_t size) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // Check if WiFi is connected first
    if (!current_wifi.wifi_connected) {
        snprintf(time_str, size, "No WiFi");
        seconds_str[0] = '\0';  // Empty string
        ampm_str[0] = '\0';     // Empty string
        return;
    }
    
    // Check if time is valid (year > 2020 means we got valid NTP time)
    if (timeinfo.tm_year < (2020 - 1900)) {
        // Check if we've been trying to sync for too long (30 seconds)
        uint32_t current_time = xTaskGetTickCount();
        if (sntp_initialized && (current_time - sntp_init_time) > pdMS_TO_TICKS(30000)) {
            ESP_LOGW(TAG, "SNTP sync timeout after 30 seconds, year=%d", timeinfo.tm_year + 1900);
            snprintf(time_str, size, "No Sync");
        } else {
            static uint32_t last_debug_time = 0;
            if (current_time - last_debug_time > pdMS_TO_TICKS(5000)) { // Log every 5 seconds
                ESP_LOGI(TAG, "Waiting for SNTP sync, current year=%d, wifi=%d, sntp_init=%d", 
                         timeinfo.tm_year + 1900, current_wifi.wifi_connected, sntp_initialized);
                last_debug_time = current_time;
            }
            snprintf(time_str, size, "Sync...");
        }
        seconds_str[0] = '\0';  // Empty string
        ampm_str[0] = '\0';     // Empty string
        return;
    } else {
        // Successfully got valid time
        static bool first_sync = true;
        if (first_sync) {
            ESP_LOGI(TAG, "SNTP sync successful! Time: %04d-%02d-%02d %02d:%02d:%02d", 
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            first_sync = false;
        }
    }
    
    // Format time in 12-hour format
    int hour_12 = timeinfo.tm_hour;
    const char* ampm = "AM";
    
    if (hour_12 == 0) {
        hour_12 = 12; // Midnight
    } else if (hour_12 > 12) {
        hour_12 -= 12;
        ampm = "PM";
    } else if (hour_12 == 12) {
        ampm = "PM"; // Noon
    }
    
    snprintf(time_str, size, "%2d:%02d", hour_12, timeinfo.tm_min);
    snprintf(seconds_str, size, "%02d", timeinfo.tm_sec);
    snprintf(ampm_str, size, "%s", ampm);
}

// Get current date
static void get_current_date(char* date_str, size_t size) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // Check if time is valid (year > 2020 means we got valid NTP time)
    if (timeinfo.tm_year < (2020 - 1900)) {
        snprintf(date_str, size, "--- -- ---");
        return;
    }
    
    // Format date (Mon DD Mon)
    const char* days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    const char* months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", 
                           "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    snprintf(date_str, size, "%s %02d %s", days[timeinfo.tm_wday], timeinfo.tm_mday, months[timeinfo.tm_mon]);
}

// Draw digital clock on line 2
static void draw_digital_clock(int x, int y) {
    char time_str[16];
    char seconds_str[8];
    char ampm_str[8];
    get_current_time_12h(time_str, seconds_str, ampm_str, sizeof(time_str));
    
    // Check if this is a status message (No WiFi, Sync..., or No Sync)
    if (strcmp(time_str, "No WiFi") == 0 || strcmp(time_str, "Sync...") == 0) {
        // Use normal 3x font (not bold) and center it for status messages
        int text_width = strlen(time_str) * 18; // Normal 3x chars are ~18 pixels wide
        int center_x = (SH1106_WIDTH - text_width) / 2;
        draw_string_3x(center_x, y, time_str);
        return;
    }
    
    // Handle flashing "No Sync" message
    if (strcmp(time_str, "No Sync") == 0) {
        // Flash rapidly - show/hide every 150ms
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        bool show_text = (current_time / 150) % 2; // Flash every 150ms
        
        if (show_text) {
            int text_width = strlen(time_str) * 18; // Normal 3x chars are ~18 pixels wide
            int center_x = (SH1106_WIDTH - text_width) / 2;
            draw_string_3x(center_x, y, time_str);
        }
        // If not showing, just leave blank space (rapid flash effect)
        return;
    }
    
    // Calculate positions for centered time + seconds/AM/PM - shifted left by 5 pixels for idle mode
    int time_width = strlen(time_str) * 19; // Bold 3x chars are ~19 pixels wide
    int seconds_width = strlen(seconds_str) * 6;  // Normal chars are ~6 pixels wide
    int ampm_width = strlen(ampm_str) * 6;  // Normal chars are ~6 pixels wide
    int side_info_width = (seconds_width > ampm_width) ? seconds_width : ampm_width; // Use wider one
    int total_width = time_width + side_info_width + 4; // 4 pixel gap between time and side info
    int start_x = ((SH1106_WIDTH - total_width) / 2) - 5; // Moved left by 5 pixels
    
    // Draw bold time
    draw_string_3x_bold(start_x, y, time_str);
    
    // Draw small seconds above AM/PM (aligned to right side)
    int side_info_x = start_x + time_width + 4;
    draw_string(side_info_x, y + 2, seconds_str); // +2 to position near top of 3x text
    
    // Draw small AM/PM below seconds (aligned to bottom of time text)
    draw_string(side_info_x, y + 14, ampm_str); // +14 to align to bottom of 3x text
}

// Update display from buffer
esp_err_t display_update(void) {
    for (int page = 0; page < 8; page++) {
        // Set page address
        sh1106_write_command(SH1106_SETPAGEADDR + page);
        // Set column address (SH1106 has 132 columns; many panels start at column 2).
        // Use runtime column offset so different panels (e.g. 1.28" SH1106 vs
        // 0.96" SSD1306-style) can be aligned via system_config.
        uint8_t low = (uint8_t)(s_display_column_offset & 0x0F);
        uint8_t high = (uint8_t)((s_display_column_offset >> 4) & 0x0F);
        sh1106_write_command(SH1106_SETLOWCOLUMN + low);
        sh1106_write_command(SH1106_SETHIGHCOLUMN + high);
        
        // Send page data
        sh1106_write_data(&display_buffer[page * SH1106_WIDTH], SH1106_WIDTH);
    }
    return ESP_OK;
}

// Initialize I2C and display with explicit I2C settings
esp_err_t display_init_with_i2c(int sda_gpio, int scl_gpio, int freq_hz) {
    if (freq_hz <= 0) {
        freq_hz = SH1106_I2C_FREQ_HZ;
    }

    // Configure I2C
    i2c_config_t i2c_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = freq_hz,
    };
    
    esp_err_t ret = i2c_param_config(SH1106_I2C_PORT, &i2c_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = i2c_driver_install(SH1106_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize SH1106
    vTaskDelay(pdMS_TO_TICKS(100)); // Power-up delay
    
    // Initialization sequence
    sh1106_write_command(SH1106_DISPLAYOFF);
    sh1106_write_command(SH1106_SETDISPLAYCLOCKDIV);
    sh1106_write_command(0x80);
    sh1106_write_command(SH1106_SETMULTIPLEX);
    sh1106_write_command(0x3F);
    sh1106_write_command(SH1106_SETDISPLAYOFFSET);
    // Vertical display offset; configurable per panel
    sh1106_write_command(SH1106_DISPLAY_OFFSET & 0x3F);
    sh1106_write_command(SH1106_SETSTARTLINE | 0x0);
    sh1106_write_command(SH1106_CHARGEPUMP);
    sh1106_write_command(0x14);
    sh1106_write_command(SH1106_MEMORYMODE);
    sh1106_write_command(0x00);
    sh1106_write_command(SH1106_SEGREMAP | 0x1);
    sh1106_write_command(SH1106_COMSCANDEC);
    sh1106_write_command(SH1106_SETCOMPINS);
    sh1106_write_command(0x12);
    sh1106_write_command(SH1106_SETCONTRAST);
    sh1106_write_command(0xCF);
    sh1106_write_command(SH1106_SETPRECHARGE);
    sh1106_write_command(0xF1);
    sh1106_write_command(SH1106_SETVCOMDETECT);
    sh1106_write_command(0x40);
    sh1106_write_command(SH1106_DISPLAYALLON_RESUME);
    sh1106_write_command(SH1106_NORMALDISPLAY);
    sh1106_write_command(SH1106_DISPLAYON);
    
    // Create mutex
    display_mutex = xSemaphoreCreateMutex();
    if (!display_mutex) {
        ESP_LOGE(TAG, "Failed to create display mutex");
        return ESP_FAIL;
    }
    
    display_initialized = true;
    ESP_LOGI(TAG, "SH1106 display initialized successfully (SDA=%d, SCL=%d, Freq=%d Hz)",
             sda_gpio, scl_gpio, freq_hz);
    return ESP_OK;
}

// Initialize I2C and display using default pins/frequency from Kconfig
esp_err_t display_init(void) {
    return display_init_with_i2c(SH1106_I2C_SDA_GPIO, SH1106_I2C_SCL_GPIO, SH1106_I2C_FREQ_HZ);
}

// Set song metadata
void display_set_song_metadata(const char* title, const char* artist, const char* album, const char* genre) {
    if (!display_initialized) return;
    
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (title && strcmp(current_metadata.title, title) != 0) {
            strncpy(current_metadata.title, title, sizeof(current_metadata.title) - 1);
            current_metadata.title_updated = true;
            title_scroll_pos = 0; // Reset scroll position for new title
        }
        if (artist && strcmp(current_metadata.artist, artist) != 0) {
            strncpy(current_metadata.artist, artist, sizeof(current_metadata.artist) - 1);
            current_metadata.artist_updated = true;
            artist_scroll_pos = 0; // Reset scroll position for new artist
        }
        if (album && strcmp(current_metadata.album, album) != 0) {
            strncpy(current_metadata.album, album, sizeof(current_metadata.album) - 1);
            current_metadata.album_updated = true;
        }
        if (genre && strcmp(current_metadata.genre, genre) != 0) {
            strncpy(current_metadata.genre, genre, sizeof(current_metadata.genre) - 1);
            current_metadata.genre_updated = true;
        }
        xSemaphoreGive(display_mutex);
    }
}

// Set audio levels
void display_set_audio_levels(const uint8_t* eq_levels, uint8_t volume, bool is_playing) {
    if (!display_initialized) return;

    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (eq_levels) {
            memcpy(current_audio.eq_levels, eq_levels, EQ_BARS);
        }
        current_audio.volume_level = volume;
        current_audio.is_playing = is_playing;
        current_audio.last_update_time = xTaskGetTickCount();
        xSemaphoreGive(display_mutex);
    }
}

// Set connection status
void display_set_connection_status(bool connected) {
    if (!display_initialized) return;
    
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        current_audio.is_connected = connected;
        xSemaphoreGive(display_mutex);
    }
}

// Set resyncing state
void display_set_resyncing(bool resyncing) {
    if (!display_initialized) return;
    
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        current_audio.is_resyncing = resyncing;
        xSemaphoreGive(display_mutex);
    }
}

// Set device name
void display_set_device_name(const char* device_name) {
    ESP_LOGI(TAG, "display_set_device_name called with: %s", device_name ? device_name : "NULL");
    ESP_LOGI(TAG, "display_initialized: %s", display_initialized ? "true" : "false");
    
    if (!display_initialized) {
        ESP_LOGW(TAG, "Display not initialized, cannot set device name");
        return;
    }
    if (!device_name) {
        ESP_LOGW(TAG, "Device name is NULL, cannot set");
        return;
    }
    
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ESP_LOGI(TAG, "Mutex acquired, updating device name from '%s' to '%s'", current_device_name, device_name);
        strncpy(current_device_name, device_name, sizeof(current_device_name) - 1);
        current_device_name[sizeof(current_device_name) - 1] = '\0';
        ESP_LOGI(TAG, "Device name updated successfully to: %s", current_device_name);
        xSemaphoreGive(display_mutex);
    } else {
        ESP_LOGW(TAG, "Failed to acquire display mutex within 100ms timeout");
    }
}

// Set WiFi signal strength
void display_set_wifi_signal(int32_t rssi, bool wifi_connected) {
    if (!display_initialized) return;
    
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        current_wifi.rssi = rssi;
        current_wifi.wifi_connected = wifi_connected;
        current_wifi.last_update_time = xTaskGetTickCount();
        
        // Convert RSSI to signal bars (0-4)
        if (!wifi_connected) {
            current_wifi.signal_bars = 0;
        } else if (rssi >= -50) {
            current_wifi.signal_bars = 4;  // Excellent
        } else if (rssi >= -60) {
            current_wifi.signal_bars = 3;  // Good
        } else if (rssi >= -70) {
            current_wifi.signal_bars = 2;  // Fair
        } else if (rssi >= -80) {
            current_wifi.signal_bars = 1;  // Poor
        } else {
            current_wifi.signal_bars = 0;  // Very poor
        }
        
        xSemaphoreGive(display_mutex);
    }
}

// Draw large WiFi signal bars (like phone signal bars)
static void draw_wifi_signal(int x, int y) {
    // Draw signal bars from x position to right edge of screen
    // Each bar gets progressively taller, spread across available width
    
    if (!current_wifi.wifi_connected) {
        // Draw "NO WiFi" text when disconnected
        draw_string(x, y, "NO WiFi");
        return;
    }
    
    // Calculate available width (screen width minus starting position minus some padding)
    int available_width = SH1106_WIDTH - x - 2;  // Leave 2 pixels padding on right
    int bar_width = (available_width - 3) / 4;   // 4 bars with 1 pixel spacing between
    
    if (bar_width < 2) bar_width = 2;  // Minimum bar width
    
    // Bar heights (getting progressively taller) - increased by 5px more each
    int bar_heights[] = {10, 11, 13, 15};
    
    // Draw 4 signal strength bars
    for (int bar = 0; bar < 4; bar++) {
        int bar_x = x + (bar * (bar_width + 1));  // 1 pixel spacing between bars
        int bar_height = bar_heights[bar];
        
        // Only draw this bar if signal strength is high enough
        bool draw_bar = (current_wifi.signal_bars > bar);
        
        // Draw the bar (filled rectangle)
        if (draw_bar) {
            for (int bx = 0; bx < bar_width; bx++) {
                for (int by = 0; by < bar_height; by++) {
                    int px = bar_x + bx;
                    int py = y + (15 - bar_height) + by;  // Align bars to bottom (y+15 is baseline for even taller bars)
                    if (px < SH1106_WIDTH && py >= 0 && py < SH1106_HEIGHT) {
                        set_pixel(px, py, true);
                    }
                }
            }
        }
    }
}

// Set Bluetooth connected device name
void display_set_bt_device_name(const char* device_name) {
    if (!display_initialized) return;
    
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (device_name && strlen(device_name) > 0) {
            strncpy(bt_connected_device_name, device_name, sizeof(bt_connected_device_name) - 1);
            bt_connected_device_name[sizeof(bt_connected_device_name) - 1] = '\0';
        } else {
            bt_connected_device_name[0] = '\0';
        }
        xSemaphoreGive(display_mutex);
    }
}

// Set Bluetooth signal strength
void display_set_bt_signal(bool connected, int32_t rssi) {
    if (!display_initialized) return;
    
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        current_bt.bt_connected = connected;
        current_bt.rssi = rssi;
        current_bt.last_update_time = xTaskGetTickCount();
        
        // Convert RSSI to signal bars based on Bluetooth behavior
        // 0 dBm = Perfect, -44 dBm = Disconnection, -40 dBm = Terrible
        if (!connected) {
            current_bt.signal_bars = 0;
        } else if (rssi >= -10) {
            current_bt.signal_bars = 4;  // Excellent (0 to -10 dBm)
        } else if (rssi >= -20) {
            current_bt.signal_bars = 3;  // Good (-11 to -20 dBm)
        } else if (rssi >= -35) {
            current_bt.signal_bars = 2;  // Fair (-21 to -35 dBm)
        } else if (rssi >= -42) {
            current_bt.signal_bars = 1;  // Poor (-36 to -42 dBm)
        } else {
            current_bt.signal_bars = 0;  // Critical (-43 dBm and below)
        }
        
        xSemaphoreGive(display_mutex);
    }
}

void display_set_bt_audio_playing(bool playing) {
    if (!display_initialized) return;
    
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        current_audio.bt_audio_playing = playing;
        xSemaphoreGive(display_mutex);
    }
}

// Set snapcast volume level (separate from bluetooth volume)
void display_set_snapcast_volume(uint8_t volume) {
    if (!display_initialized) return;
    
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        snapcast_volume_level = volume;
        xSemaphoreGive(display_mutex);
    }
}

// Set snapcast mute state
void display_set_snapcast_mute(bool muted) {
    if (!display_initialized) return;
    
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        snapcast_muted = muted;
        xSemaphoreGive(display_mutex);
    }
}

// Main display task
static void display_task(void* pvParameters) {
    ESP_LOGI(TAG, "Display task started");
    
    TickType_t last_wake_time = xTaskGetTickCount();
    // Display refresh rate (controls EQ bar and overall redraw rate)
    const TickType_t frequency = pdMS_TO_TICKS(200); // ~4 FPS
    
    while (1) {
        vTaskDelayUntil(&last_wake_time, frequency);
        
        if (!display_initialized) continue;
        
        // Update scroll positions
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (current_time - last_scroll_time >= SCROLL_SPEED_MS) {
            title_scroll_pos++;
            artist_scroll_pos++;
            device_name_scroll_pos++;  // Add device name scrolling
            last_scroll_time = current_time;
        }
        
        // Clear display buffer (doesn't need mutex)
        display_clear();
        
        if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            // Bluetooth audio playing: Show source device name + song info + EQ
            if (current_bt.bt_connected && current_audio.bt_audio_playing) {
                // Device name scrolls between left edge and clock area
                const int max_bt_name_chars = 10; // roughly up to clock_x in 6px font
                const char *name = strlen(bt_connected_device_name) > 0
                                   ? bt_connected_device_name
                                   : current_device_name;
                if ((int)strlen(name) <= max_bt_name_chars) {
                    draw_string(0, 0, name);
                } else {
                    draw_scrolling_text(0, 0, name, &device_name_scroll_pos);
                }

                // Time display on the right (drawn after name so it never gets overwritten)
                char time_str[16], seconds_str[8], ampm_str[4];
                char full_time_str[20];
                get_current_time_12h(time_str, seconds_str, ampm_str, sizeof(time_str));
                snprintf(full_time_str, sizeof(full_time_str), "%s%s", time_str, ampm_str);
                // Draw clock near the right edge
                int clock_x = 80;
                draw_string(clock_x, 0, full_time_str);
                
                // Song metadata with scrolling
                if (strlen(current_metadata.title) > 0) {
                    draw_scrolling_text(0, 9, current_metadata.title, &title_scroll_pos);
                } else {
                    draw_string(0, 9, "No Title");
                }
                if (strlen(current_metadata.artist) > 0) {
                    draw_scrolling_text(0, 18, current_metadata.artist, &artist_scroll_pos);
                } else {
                    draw_string(0, 18, "No Artist");
                }
                
                // EQ visualizer scaled by volume
                for (int i = 0; i < EQ_BARS; i++) {
                    int x = i * (EQ_BAR_WIDTH + EQ_BAR_SPACING);
                    int eq_level = current_audio.eq_levels[i];
                    if (eq_level > 100) eq_level = 100;
                    int volume_scale = (current_audio.volume_level * 80) / 100 + 20;
                    int scaled_height = (eq_level * volume_scale) / 100;
                    draw_eq_bar(x, scaled_height);
                }
            }
            // Snapclient mode: Only show when NOT in bluetooth audio mode
            else {
                // Always show volume bar or MUTED text on left side in snapclient mode
                // Use snapcast volume and mute state, not bluetooth volume
                draw_volume_bar_or_muted(0, 11, SH1106_HEIGHT - 1, snapcast_volume_level, snapcast_muted);
                
                // Device name display when idle (not connected OR muted OR resyncing)
                if (!current_audio.is_connected || snapcast_volume_level == 0 || current_audio.is_resyncing) {
                // Initialize SNTP for time synchronization (needed for idle screen clock)
                initialize_sntp();
                
                // Date in left corner of idle screen - shifted right to avoid volume bar
                char date_str[16];
                get_current_date(date_str, sizeof(date_str));
                draw_string(7, 0, date_str); // Shifted right by 7 pixels for 5-pixel wide volume bar
                
                // Device name 2x larger (slightly smaller) with scrolling if needed - shifted right
                int text_width = strlen(current_device_name) * 12; // 2x chars are ~12 pixels wide
                int center_x = 7 + (SH1106_WIDTH - 7 - text_width) / 2; // Center in remaining space after volume bar
                int center_y = 20; // Positioned for 2x font
                if (center_x < 7) center_x = 7; // Don't go behind volume bar
                
                // Use 2x scrolling for slightly smaller device name
                draw_scrolling_text_2x(center_x, center_y, current_device_name, &device_name_scroll_pos);
                
                // Digital clock below device name on idle screen - shifted right to avoid volume bar
                draw_digital_clock(7, 38); // Shifted right by 7 pixels for 5-pixel wide volume bar
                
                    // WiFi signal indicator (top right area on idle screen) - keep larger bars
                    draw_wifi_signal(90, 0);  // Back to original position
                } else {
                    // Device name 1.5x with scrolling - shifted right to avoid volume bar
                    int max_device_name_width = 88 - 7; // Leave space for volume bar and gap before WiFi bars
                    int device_name_chars_that_fit = max_device_name_width / 9; // 1.5x chars are ~9 pixels wide
                    
                    if (strlen(current_device_name) <= device_name_chars_that_fit) {
                        // Device name fits - draw at 1.5x size, shifted right
                        draw_string_1_5x(7, 0, current_device_name);
                    } else {
                        // Device name too long - use 1.5x scrolling, shifted right
                        draw_scrolling_text_1_5x(7, 0, current_device_name, &device_name_scroll_pos);
                    }
                }
                
                // Show additional UI elements only when connected AND not muted AND not resyncing
                if (current_audio.is_connected && snapcast_volume_level > 0 && !current_audio.is_resyncing) {
                    // Initialize SNTP when we have network connectivity
                    initialize_sntp();
                    
                    // This volume bar is now handled above in the snapclient section
                    // draw_volume_bar_or_muted(0, 11, SH1106_HEIGHT - 1, snapcast_volume_level, snapcast_muted);
                    
                    // WiFi signal indicator (positioned with more space for device name)
                    draw_wifi_signal(90, 0);
                    
                    // Digital clock display on line 2 - shifted right to avoid volume bar
                    draw_digital_clock(7, 21); // Moved to X=7 to clear 5-pixel volume bar
                    
                    // Volume indicator (lowered by another half line and smaller font) - shifted right
                    char vol_str[16];
                    snprintf(vol_str, sizeof(vol_str), "%d", snapcast_volume_level);
                    // Use smaller font by drawing individual characters at half size (simulate smaller font)
                    // For now, move it down another half line to y=35 and shift right
                    draw_string(7, 35, vol_str); // Moved to X=7 to clear 5-pixel volume bar
                } // End of is_connected check
            } // End of snapclient else block
            
            xSemaphoreGive(display_mutex);
        }
        
        // Update display (always update, even if mutex wasn't acquired)
        display_update();
    }
}

// Start display task
void display_task_start(void) {
    if (display_task_handle == NULL) {
        xTaskCreate(display_task, "display_task", 4096, NULL, 5, &display_task_handle);
        ESP_LOGI(TAG, "Display task started");
    }
}

// Stop display task
void display_task_stop(void) {
    if (display_task_handle != NULL) {
        vTaskDelete(display_task_handle);
        display_task_handle = NULL;
        ESP_LOGI(TAG, "Display task stopped");
    }
}