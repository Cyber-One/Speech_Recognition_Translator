#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "ff.h"
#include "diskio.h"

// ==============================
// I2C Configuration (Stage 2 read)
// ==============================
#define I2C_STAGE2_PORT i2c0
#define I2C_STAGE2_SDA 20
#define I2C_STAGE2_SCL 21
#define I2C_STAGE2_BAUD 400000

#define STAGE2_BASE_ADDR 0x60
#define STAGE2_COUNT 5
#define STAGE4_ADDR 0x65

// Stage 2 registers
#define STAGE2_REG_CONTROL    0x00
#define STAGE2_REG_FIFO_LEN   0x01
#define STAGE2_REG_FIFO_READ  0x05
#define STAGE2_REG_TARGET_NEURON 0x04
#define STAGE2_REG_PAGE_MODE  0x0C
#define STAGE2_REG_PAGE_ADDR  0x0D
#define STAGE2_REG_PAGE_LEN   0x0E
#define STAGE2_REG_PAGE_DATA  0x0F
#define STAGE2_REG_LAST_MAX_ID 0x10
#define STAGE2_REG_LAST_MAX_VAL 0x11
#define STAGE2_REG_LAST_TARGET_VAL 0x12
#define STAGE2_REG_LAST_USER_ID 0x13
#define STAGE2_REG_LAST_USER_VAL 0x14
#define STAGE2_REG_LAST_FEMALE_VAL 0x15
#define STAGE2_REG_LAST_MALE_VAL 0x16

// Stage 2 page modes
#define STAGE2_PAGE_NONE  0x00
#define STAGE2_PAGE_W1    0x01
#define STAGE2_PAGE_B1    0x02
#define STAGE2_PAGE_W2    0x03
#define STAGE2_PAGE_B2    0x04
#define STAGE2_PAGE_INPUT 0x05

// Stage 2 control bits (write 0x06 to freeze + pause)
#define STAGE2_CTRL_FREEZE_PAUSE 0x0006
#define STAGE2_CTRL_BACKPROP 0x0004

// Stage 4 registers (Speech_Generation)
#define STAGE4_REG_CONTROL_STATUS 0x00
#define STAGE4_REG_IMAGE_LINE_PTR 0x10
#define STAGE4_REG_IMAGE_DATA 0x11
#define STAGE4_REG_GEN_PHONEME 0x12
#define STAGE4_REG_GEN_COMMAND 0x13
#define STAGE4_REG_TRAIN_FEEDBACK 0x14
#define STAGE4_REG_TRAIN_TARGET 0x15

#define STAGE4_CMD_GENERATE_IMAGE 0x01
#define STAGE4_CMD_BACKPROP_STEP 0x02
#define STAGE4_CMD_RESET_IMAGE_PTR 0x04

#define STAGE4_IMAGE_BINS 40
#define STAGE4_IMAGE_LINES 100

// ==============================
// LCD (PCF8574, HD44780, 20x4)
// ==============================
#define LCD_I2C_PORT I2C_STAGE2_PORT
#define LCD_I2C_ADDR 0x27
#define LCD_BACKLIGHT 0x08
#define LCD_EN 0x04
#define LCD_RW 0x02
#define LCD_RS 0x01

// ==============================
// Keypad (PCF8574, 4x4 matrix)
// ==============================
#define KEYPAD_I2C_PORT I2C_STAGE2_PORT
#define KEYPAD_I2C_ADDR 0x26

// PCF8574 bit mapping: P0..P3 = rows (outputs), P4..P7 = cols (inputs)
#define KEYPAD_ROW_MASK 0x0F
#define KEYPAD_COL_MASK 0xF0

static const char keypad_map[4][4] = {
    {'1','2','3','A'},
    {'4','5','6','B'},
    {'7','8','9','C'},
    {'*','0','#','D'}
};

typedef enum {
    MENU_SCREEN0 = 0,
    MENU_MAIN,
    MENU_SELECT_USER,
    MENU_NEW_USER,
    MENU_TRAIN_CAPTURE,
    MENU_SELECT_UNREC,
    MENU_STAGE2_ANN_CONFIRM,
    MENU_SAVE_ANN_CONFIRM,
    MENU_LOAD_ANN_SELECT,
    MENU_SPEECH_GEN_TRAIN
} menu_state_t;

static menu_state_t menu_state = MENU_SCREEN0;
static char input_buffer_line[32];
static uint8_t input_len = 0;
static uint8_t menu_main_page = 0;

#define ADD_USER_NAME_MAX 14
#define USER_MENU_MAX 20
#define WORD_TEXT_MAX 27

typedef enum {
    ADD_USER_STEP_NAME = 0,
    ADD_USER_STEP_GENDER,
    ADD_USER_STEP_LANGUAGE
} add_user_step_t;

static add_user_step_t add_user_step = ADD_USER_STEP_NAME;
static uint8_t add_user_id = 1;
static char add_user_name[ADD_USER_NAME_MAX + 1] = "A";
static uint8_t add_user_name_len = 1;
static uint8_t add_user_cursor = 0;
static bool add_user_gender_male = true;
static uint8_t add_user_lang_index = 1;
static char add_user_language[31] = "English";

static uint8_t user_menu_ids[USER_MENU_MAX] = {0};
static char user_menu_names[USER_MENU_MAX][26];
static uint8_t user_menu_count = 0;
static uint8_t user_menu_index = 0;

#define ANN_VERSION_MAX 100
static uint8_t ann_versions[ANN_VERSION_MAX] = {0};
static uint8_t ann_version_count = 0;
static uint8_t ann_version_index = 0;

// LCD history/state
#define WORD_HISTORY_COUNT 10
static char word_history[WORD_HISTORY_COUNT][WORD_TEXT_MAX];
static uint8_t word_history_count = 0;
static char lcd_status_line[21] = "Status: Booting";

#define UNREC_PREVIEW_COUNT 3
static char unrec_preview[UNREC_PREVIEW_COUNT][WORD_TEXT_MAX];
static uint8_t unrec_preview_count = 0;

// Silence IDs
#define SIL_WORD_ID 0x02
#define SIL_SENTENCE_ID 0x03

// ==============================
// Output Selection (2 GPIOs)
// ==============================
#define MODE_SEL0_GPIO 2
#define MODE_SEL1_GPIO 3

typedef enum {
    OUTPUT_USB = 0,
    OUTPUT_TTL = 1,
    OUTPUT_I2C = 2
} output_mode_t;

// ==============================
// Word-ready inputs (1 per stage 2)
// ==============================
static const uint8_t word_ready_pins[STAGE2_COUNT] = {6, 7, 8, 9, 10};

// ==============================
// Diagnostics: stage2 not running
// ==============================
static const uint8_t stage2_fault_pins[STAGE2_COUNT] = {11, 12, 13, 14, 15};

// ==============================
// SPI microSD (placeholder)
// ==============================
#define SD_SPI_PORT spi0
#define SD_SPI_SCK 18
#define SD_SPI_TX 19
#define SD_SPI_RX 16
#define SD_SPI_CS 17

// ==============================
// TTL Serial (UART0)
// ==============================
#define TTL_UART uart0
#define TTL_UART_TX 0
#define TTL_UART_RX 1
#define TTL_BAUD 115200

// ==============================
// Data Structures
// ==============================
typedef struct {
    uint8_t max_id;
    uint8_t max_val;
    uint8_t female_val;
    uint8_t male_val;
    uint8_t user_id;
} stage2_entry_t;

#define PHONEME_SEQ_LEN 15

// Dictionary text format (fixed width, binary-search friendly):
// 15 hex bytes with trailing spaces (45 chars) + 2-char language ID + space + 26-char word + CRLF (2 chars)
// Example:
// "05 10 15 21 00 00 00 00 00 00 00 00 00 00 00 00 hello                     \r\n"
#define DICT_HEX_FIELD_CHARS 45
#define DICT_LANG_ID_CHARS 2
#define DICT_LANG_SEP_CHARS 1
#define DICT_LANG_OFFSET DICT_HEX_FIELD_CHARS
#define DICT_WORD_OFFSET (DICT_HEX_FIELD_CHARS + DICT_LANG_ID_CHARS + DICT_LANG_SEP_CHARS)
#define DICT_WORD_SIZE 26
#define DICT_LINE_END_CHARS 2
#define DICT_RECORD_SIZE (DICT_HEX_FIELD_CHARS + DICT_LANG_ID_CHARS + DICT_LANG_SEP_CHARS + DICT_WORD_SIZE + DICT_LINE_END_CHARS)

// Language file format: "HH Name\r\n" (2-digit hex ID, space, text name)
#define LANG_RECORD_SIZE 32
#define LANG_ID_SIZE 2
#define LANG_NAME_SIZE 30

// Language IDs
#define LANG_UNKNOWN 0
#define LANG_ENGLISH 1

#define USER_ID_UNKNOWN 0
#define USER_ID_MAX 20

// Stage 2 network dimensions
#define INPUT_NEURONS 41
#define HIDDEN_NEURONS 100
#define OUTPUT_NEURONS 200

#define W1_SIZE (HIDDEN_NEURONS * INPUT_NEURONS)
#define B1_SIZE (HIDDEN_NEURONS)
#define W2_SIZE (OUTPUT_NEURONS * HIDDEN_NEURONS)
#define B2_SIZE (OUTPUT_NEURONS)
#define NN_TOTAL_SIZE (W1_SIZE + B1_SIZE + W2_SIZE + B2_SIZE)

// ==============================
// Training configuration
// ==============================
#define TRAIN_BEAM_INDEX 2
#define INPUT_PERIOD_MS 16
#define PEAK_WINDOW_SECONDS 2
#define PEAK_WINDOW_FRAMES (PEAK_WINDOW_SECONDS * 1000 / INPUT_PERIOD_MS)
#define CAPTURE_FRAMES 100
#define CAPTURE_FRAME_BYTES 40
#define MAX_WORD_LEN 24
#define MAX_PHONEMES_PER_WORD 8
#define TRAIN_WORDS_MAX 120
#define TRAIN_MIN_SPOKEN_FRAMES 6
#define STAGE2_CERTAINTY_THRESHOLD 204
#define STAGE2_ANN_MAX_EPOCHS 20
#define STAGE4_TRAIN_MAX_EPOCHS 20

// Forward declaration of training state
typedef enum {
    TRAIN_IDLE = 0,
    TRAIN_WAIT_TRIGGER,
    TRAIN_CAPTURE,
    TRAIN_SAVE
} train_state_t;

static train_state_t train_state = TRAIN_IDLE;

typedef struct {
    char username[32];
    char full_name[64];
    uint8_t user_id;
    uint8_t age;
    char gender[8];
    char language[31];
    bool set;
} user_profile_t;

static user_profile_t current_user = {0};

static char training_words[TRAIN_WORDS_MAX][DICT_WORD_SIZE + 1];
static uint16_t training_word_count = 0;
static uint16_t training_word_index = 0;
static bool training_words_loaded = false;

typedef struct {
    uint8_t seq[PHONEME_SEQ_LEN];
    uint8_t count;
} beam_seq_t;

static beam_seq_t beam_sequences[STAGE2_COUNT];

// ==============================
// Output helpers
// ==============================
static output_mode_t read_output_mode(void) {
    bool m0 = gpio_get(MODE_SEL0_GPIO);
    bool m1 = gpio_get(MODE_SEL1_GPIO);
    uint8_t mode = (m1 << 1) | (m0 << 0);
    switch (mode) {
        case 0: return OUTPUT_USB;
        case 1: return OUTPUT_TTL;
        case 2: return OUTPUT_I2C;
        default: return OUTPUT_USB;
    }
}

static void output_send_line(const char *line) {
    output_mode_t mode = read_output_mode();
    switch (mode) {
        case OUTPUT_TTL:
            uart_puts(TTL_UART, line);
            uart_puts(TTL_UART, "\r\n");
            break;
        case OUTPUT_I2C:
            // TODO: implement upstream I2C output
            break;
        case OUTPUT_USB:
        default:
            printf("%s\n", line);
            break;
    }
}

// ==============================
// LCD helpers
// ==============================
static void lcd_i2c_write(uint8_t data) {
    uint8_t buf = (uint8_t)(data | LCD_BACKLIGHT);
    i2c_write_blocking(LCD_I2C_PORT, LCD_I2C_ADDR, &buf, 1, false);
}

static void lcd_pulse_enable(uint8_t data) {
    lcd_i2c_write((uint8_t)(data | LCD_EN));
    sleep_us(1);
    lcd_i2c_write((uint8_t)(data & ~LCD_EN));
    sleep_us(50);
}

static void lcd_write4(uint8_t nibble, bool rs) {
    uint8_t data = (uint8_t)((nibble << 4) | (rs ? LCD_RS : 0));
    lcd_pulse_enable(data);
}

static void lcd_command(uint8_t cmd) {
    lcd_write4((uint8_t)(cmd >> 4), false);
    lcd_write4((uint8_t)(cmd & 0x0F), false);
    sleep_us(50);
}

static void lcd_write_char(char c) {
    lcd_write4((uint8_t)(c >> 4), true);
    lcd_write4((uint8_t)(c & 0x0F), true);
}

static void lcd_clear(void) {
    lcd_command(0x01);
    sleep_ms(2);
}

static void lcd_set_cursor(uint8_t col, uint8_t row) {
    static const uint8_t row_offsets[4] = {0x00, 0x40, 0x14, 0x54};
    lcd_command((uint8_t)(0x80 | (row_offsets[row % 4] + col)));
}

static void lcd_print(const char *s) {
    while (*s) {
        lcd_write_char(*s++);
    }
}

static void lcd_init(void) {
    sleep_ms(50);
    lcd_write4(0x03, false);
    sleep_ms(5);
    lcd_write4(0x03, false);
    sleep_us(150);
    lcd_write4(0x03, false);
    lcd_write4(0x02, false);

    lcd_command(0x28); // 4-bit, 2 line, 5x8
    lcd_command(0x0C); // display on, cursor off
    lcd_command(0x06); // entry mode
    lcd_clear();
}

static bool lcd_available_for_status(void) {
    return (train_state == TRAIN_IDLE) && (menu_state == MENU_SCREEN0);
}

static void lcd_print_padded_line(uint8_t row, const char *text) {
    char line[21];
    memset(line, ' ', 20);
    line[20] = '\0';
    if (text) {
        strncpy(line, text, 20);
    }
    lcd_set_cursor(0, row);
    lcd_print(line);
}

static void lcd_set_status(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(lcd_status_line, sizeof(lcd_status_line), fmt, args);
    va_end(args);

    lcd_status_line[20] = '\0';
}

static void lcd_render_screen0(void) {
    if (!lcd_available_for_status()) return;

    lcd_clear();
    lcd_print_padded_line(0, lcd_status_line);

    char history_text[256] = {0};
    size_t cursor = 0;

    int start = (word_history_count > WORD_HISTORY_COUNT) ? (word_history_count - WORD_HISTORY_COUNT) : 0;
    for (int i = start; i < word_history_count; i++) {
        const char *word = word_history[i % WORD_HISTORY_COUNT];
        if (word[0] == '\0') continue;

        if (cursor > 0 && cursor + 1 < sizeof(history_text) - 1) {
            history_text[cursor++] = ' ';
        }

        size_t wlen = strnlen(word, DICT_WORD_SIZE);
        if (cursor + wlen >= sizeof(history_text) - 1) {
            break;
        }

        memcpy(&history_text[cursor], word, wlen);
        cursor += wlen;
        history_text[cursor] = '\0';
    }

    for (uint8_t row = 1; row <= 3; row++) {
        char line[21] = {0};
        size_t offset = (size_t)(row - 1) * 20;
        if (offset < strlen(history_text)) {
            strncpy(line, &history_text[offset], 20);
        }
        lcd_print_padded_line(row, line);
    }
}

static void word_history_push(const char *word) {
    if (!word || word[0] == '\0') return;

    if (word_history_count < WORD_HISTORY_COUNT) {
        strncpy(word_history[word_history_count], word, sizeof(word_history[word_history_count]) - 1);
        word_history[word_history_count][sizeof(word_history[word_history_count]) - 1] = '\0';
        word_history_count++;
    } else {
        for (int i = 0; i < WORD_HISTORY_COUNT - 1; i++) {
            strncpy(word_history[i], word_history[i + 1], sizeof(word_history[i]) - 1);
            word_history[i][sizeof(word_history[i]) - 1] = '\0';
        }

        strncpy(word_history[WORD_HISTORY_COUNT - 1], word, sizeof(word_history[WORD_HISTORY_COUNT - 1]) - 1);
        word_history[WORD_HISTORY_COUNT - 1][sizeof(word_history[WORD_HISTORY_COUNT - 1]) - 1] = '\0';
    }

    lcd_render_screen0();
}

static int strcasecmp_local(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (char)tolower((unsigned char)*a++);
        char cb = (char)tolower((unsigned char)*b++);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
    }
    return (int)(unsigned char)tolower((unsigned char)*a) - (int)(unsigned char)tolower((unsigned char)*b);
}

// ==============================
// Keypad helpers
// ==============================
static uint8_t keypad_read_raw(void) {
    uint8_t data = 0xFF;
    i2c_read_blocking(KEYPAD_I2C_PORT, KEYPAD_I2C_ADDR, &data, 1, false);
    return data;
}

static void keypad_write(uint8_t value) {
    i2c_write_blocking(KEYPAD_I2C_PORT, KEYPAD_I2C_ADDR, &value, 1, false);
}

static char keypad_get_key(void) {
    for (int row = 0; row < 4; row++) {
        uint8_t row_mask = (uint8_t)(~(1u << row) & KEYPAD_ROW_MASK);
        uint8_t out = (uint8_t)(row_mask | KEYPAD_COL_MASK);
        keypad_write(out);
        sleep_us(50);

        uint8_t data = keypad_read_raw();
        uint8_t cols = (uint8_t)(~data & KEYPAD_COL_MASK);
        if (cols) {
            for (int col = 0; col < 4; col++) {
                if (cols & (1u << (col + 4))) {
                    return keypad_map[row][col];
                }
            }
        }
    }
    return 0;
}

static void menu_render_screen0(void) {
    lcd_render_screen0();
}

static void menu_render_main(void) {
    lcd_clear();
    char title[21];
    snprintf(title, sizeof(title), "Main Menu Pg %u", (unsigned)menu_main_page);
    lcd_print_padded_line(0, title);

    if (menu_main_page == 0) {
        lcd_print_padded_line(1, "1:Add New User");
        lcd_print_padded_line(2, "2:Sel User 3:Train");
        lcd_print_padded_line(3, "B:Pg1  *:Exit");
    } else if (menu_main_page == 1) {
        lcd_print_padded_line(1, "4:Unrec 5:SpGen");
        lcd_print_padded_line(2, "6:Stage2 ANN Trn");
        lcd_print_padded_line(3, "A:Pg0 B:Pg2");
    } else if (menu_main_page == 2) {
        lcd_print_padded_line(1, "7:Save ANN");
        lcd_print_padded_line(2, "8:Load ANN");
        lcd_print_padded_line(3, "A:Pg1  *:Exit");
    } else {
        menu_main_page = 2;
        lcd_print_padded_line(1, "7:Save ANN");
        lcd_print_padded_line(2, "8:Load ANN");
        lcd_print_padded_line(3, "A:Pg1  *:Exit");
    }
}

static void menu_render_stage2_ann_confirm(void) {
    lcd_clear();
    lcd_print_padded_line(0, "Stage 2 ANN Train");
    lcd_print_padded_line(1, "Are you sure?");
    lcd_print_padded_line(2, "#:Yes");
    lcd_print_padded_line(3, "*:No");
}

static void menu_render_save_ann_confirm(void) {
    lcd_clear();
    lcd_print_padded_line(0, "Save ANN");
    lcd_print_padded_line(1, "Are you sure?");
    lcd_print_padded_line(2, "#:Yes");
    lcd_print_padded_line(3, "*:No");
}

static void menu_render_load_ann_select(void) {
    lcd_clear();
    lcd_print_padded_line(0, "Load Speech ANN");

    if (ann_version_count == 0) {
        lcd_print_padded_line(1, "No saved ANN files");
        lcd_print_padded_line(2, "");
        lcd_print_padded_line(3, "*:Back");
        return;
    }

    uint8_t version = ann_versions[ann_version_index];
    char line1[21];
    snprintf(line1, sizeof(line1), "Sel: ANN v%02u", (unsigned)version);
    lcd_print_padded_line(1, line1);

    char line2[21];
    snprintf(line2,
             sizeof(line2),
             "%u/%u",
             (unsigned)(ann_version_index + 1),
             (unsigned)ann_version_count);
    lcd_print_padded_line(2, line2);
    lcd_print_padded_line(3, "A/B:Sel #:Load *:Bk");
}

static void menu_render_load_ann_progress(uint8_t version, uint8_t step, uint8_t total_steps) {
    lcd_clear();
    char line0[21];
    snprintf(line0, sizeof(line0), "Load ANN v%02u", (unsigned)version);
    lcd_print_padded_line(0, line0);

    char line1[21];
    snprintf(line1, sizeof(line1), "Device %u/%u", (unsigned)step, (unsigned)total_steps);
    lcd_print_padded_line(1, line1);

    uint8_t progress = (total_steps > 0) ? (uint8_t)((step * 100u) / total_steps) : 0;
    char line2[21];
    snprintf(line2, sizeof(line2), "Progress:%3u%%", (unsigned)progress);
    lcd_print_padded_line(2, line2);
    lcd_print_padded_line(3, "Please wait...");
}

static void menu_render_save_ann_progress(uint8_t version, const char *phase, uint8_t progress_pct) {
    lcd_clear();
    char line0[21];
    snprintf(line0, sizeof(line0), "Save ANN v%02u", (unsigned)version);
    lcd_print_padded_line(0, line0);

    char line1[21];
    snprintf(line1, sizeof(line1), "%s", (phase && phase[0]) ? phase : "Working");
    lcd_print_padded_line(1, line1);

    char line2[21];
    snprintf(line2, sizeof(line2), "Progress:%3u%%", (unsigned)progress_pct);
    lcd_print_padded_line(2, line2);
    lcd_print_padded_line(3, "Please wait...");
}

static char add_user_next_char(char c) {
    if (c < 'A' || c > 'Z') return 'A';
    return (c == 'Z') ? 'A' : (char)(c + 1);
}

static char add_user_prev_char(char c) {
    if (c < 'A' || c > 'Z') return 'A';
    return (c == 'A') ? 'Z' : (char)(c - 1);
}

static int language_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

static bool language_parse_line(const char *line, uint8_t *id_out, char *name_out, size_t name_out_len) {
    if (!line || !id_out || !name_out || name_out_len < 2) return false;

    if (!isxdigit((unsigned char)line[0]) || !isxdigit((unsigned char)line[1]) || line[2] != ' ') {
        return false;
    }

    int hi = language_hex_nibble(line[0]);
    int lo = language_hex_nibble(line[1]);
    if (hi < 0 || lo < 0) return false;

    *id_out = (uint8_t)((hi << 4) | lo);

    const char *name = &line[3];
    while (*name == ' ') name++;

    strncpy(name_out, name, name_out_len - 1);
    name_out[name_out_len - 1] = '\0';

    char *eol = strpbrk(name_out, "\r\n");
    if (eol) *eol = '\0';

    size_t nlen = strlen(name_out);
    while (nlen > 0 && name_out[nlen - 1] == ' ') {
        name_out[nlen - 1] = '\0';
        nlen--;
    }

    return name_out[0] != '\0';
}

static bool language_name_from_index(uint8_t index, char *name_out, size_t name_out_len) {
    if (!name_out || name_out_len < 2) return false;

    FIL lang_file;
    if (f_open(&lang_file, "0:/microsd/Language.dat", FA_READ | FA_OPEN_EXISTING) != FR_OK) {
        strncpy(name_out, "English", name_out_len - 1);
        name_out[name_out_len - 1] = '\0';
        return false;
    }

    char line[96];
    bool found = false;
    while (f_gets(line, sizeof(line), &lang_file)) {
        uint8_t parsed_id = 0;
        char parsed_name[LANG_NAME_SIZE + 1] = {0};
        if (!language_parse_line(line, &parsed_id, parsed_name, sizeof(parsed_name))) continue;
        if (parsed_id != index) continue;

        strncpy(name_out, parsed_name, name_out_len - 1);
        name_out[name_out_len - 1] = '\0';
        found = true;
        break;
    }

    f_close(&lang_file);

    if (!found) {
        strncpy(name_out, "English", name_out_len - 1);
        name_out[name_out_len - 1] = '\0';
    }

    return found;
}

static uint8_t language_id_from_name(const char *name) {
    if (!name || name[0] == '\0') return LANG_UNKNOWN;

    FIL lang_file;
    if (f_open(&lang_file, "0:/microsd/Language.dat", FA_READ | FA_OPEN_EXISTING) != FR_OK) {
        return LANG_UNKNOWN;
    }

    char line[96];
    uint8_t found_id = LANG_UNKNOWN;

    while (f_gets(line, sizeof(line), &lang_file)) {
        uint8_t parsed_id = 0;
        char parsed_name[LANG_NAME_SIZE + 1] = {0};
        if (!language_parse_line(line, &parsed_id, parsed_name, sizeof(parsed_name))) continue;
        if (strcasecmp_local(parsed_name, name) != 0) continue;

        found_id = parsed_id;
        break;
    }

    f_close(&lang_file);
    return found_id;
}

static uint8_t language_record_count(void) {
    FIL lang_file;
    FRESULT res = f_open(&lang_file, "0:/microsd/Language.dat", FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK) return 20;

    uint32_t count = 0;
    char line[96];
    while (f_gets(line, sizeof(line), &lang_file)) {
        uint8_t parsed_id = 0;
        char parsed_name[LANG_NAME_SIZE + 1] = {0};
        if (!language_parse_line(line, &parsed_id, parsed_name, sizeof(parsed_name))) continue;
        count++;
    }

    f_close(&lang_file);

    if (count == 0) return 20;
    if (count > 255) return 255;
    return (uint8_t)count;
}

static bool user_list_find_first_available(uint8_t *id_out) {
    if (!id_out) return false;

    bool present[USER_ID_MAX + 1] = {0};
    char names[USER_ID_MAX + 1][32];
    memset(names, 0, sizeof(names));

    FIL user_file;
    FRESULT res = f_open(&user_file, "0:/microsd/UserList.txt", FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK) {
        *id_out = 1;
        return true;
    }

    char line[96];
    while (f_gets(line, sizeof(line), &user_file)) {
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;

        char *comma = strchr(line, ',');
        if (!comma) continue;
        *comma = '\0';
        int id = atoi(line);
        if (id < 0 || id > USER_ID_MAX) continue;

        char *name = comma + 1;
        char *eol = strpbrk(name, "\r\n");
        if (eol) *eol = '\0';

        present[id] = true;
        strncpy(names[id], name, sizeof(names[id]) - 1);
        names[id][sizeof(names[id]) - 1] = '\0';
    }
    f_close(&user_file);

    for (uint8_t id = 1; id <= USER_ID_MAX; id++) {
        char default_name[16];
        snprintf(default_name, sizeof(default_name), "User%02u", id);

        if (!present[id] || names[id][0] == '\0' || strcmp(names[id], default_name) == 0) {
            *id_out = id;
            return true;
        }
    }

    return false;
}

static bool user_list_set_name(uint8_t user_id, const char *name) {
    if (user_id == 0 || user_id > USER_ID_MAX || !name || name[0] == '\0') return false;

    char names[USER_ID_MAX + 1][32];
    memset(names, 0, sizeof(names));

    FIL user_file;
    FRESULT res = f_open(&user_file, "0:/microsd/UserList.txt", FA_READ | FA_OPEN_EXISTING);
    if (res == FR_OK) {
        char line[96];
        while (f_gets(line, sizeof(line), &user_file)) {
            if (line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;

            char *comma = strchr(line, ',');
            if (!comma) continue;
            *comma = '\0';
            int id = atoi(line);
            if (id < 0 || id > USER_ID_MAX) continue;

            char *old_name = comma + 1;
            char *eol = strpbrk(old_name, "\r\n");
            if (eol) *eol = '\0';

            strncpy(names[id], old_name, sizeof(names[id]) - 1);
            names[id][sizeof(names[id]) - 1] = '\0';
        }
        f_close(&user_file);
    }

    strncpy(names[user_id], name, sizeof(names[user_id]) - 1);
    names[user_id][sizeof(names[user_id]) - 1] = '\0';

    res = f_open(&user_file, "0:/microsd/UserList.txt", FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) return false;

    UINT bw;
    const char *header = "# id,name\r\n0,Unknown\r\n";
    if (f_write(&user_file, header, (UINT)strlen(header), &bw) != FR_OK) {
        f_close(&user_file);
        return false;
    }

    for (uint8_t id = 1; id <= USER_ID_MAX; id++) {
        char line[64];
        if (names[id][0] == '\0') {
            snprintf(names[id], sizeof(names[id]), "User%02u", id);
        }
        int n = snprintf(line, sizeof(line), "%u,%s\r\n", id, names[id]);
        if (f_write(&user_file, line, (UINT)n, &bw) != FR_OK || bw != (UINT)n) {
            f_close(&user_file);
            return false;
        }
    }

    f_close(&user_file);
    return true;
}

static bool user_name_is_assigned(uint8_t id, const char *name) {
    if (id == 0 || id > USER_ID_MAX || !name || name[0] == '\0') return false;

    char default_name[16];
    snprintf(default_name, sizeof(default_name), "User%02u", id);
    return strcmp(name, default_name) != 0;
}

static void user_menu_load_assigned(void) {
    user_menu_count = 0;
    user_menu_index = 0;

    FIL user_file;
    FRESULT res = f_open(&user_file, "0:/microsd/UserList.txt", FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK) {
        return;
    }

    char line[96];
    while (f_gets(line, sizeof(line), &user_file)) {
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;

        char *comma = strchr(line, ',');
        if (!comma) continue;
        *comma = '\0';

        int id = atoi(line);
        if (id <= 0 || id > USER_ID_MAX) continue;

        char *name = comma + 1;
        char *eol = strpbrk(name, "\r\n");
        if (eol) *eol = '\0';

        if (!user_name_is_assigned((uint8_t)id, name)) continue;
        if (user_menu_count >= USER_ID_MAX) break;

        user_menu_ids[user_menu_count] = (uint8_t)id;
        strncpy(user_menu_names[user_menu_count], name, sizeof(user_menu_names[user_menu_count]) - 1);
        user_menu_names[user_menu_count][sizeof(user_menu_names[user_menu_count]) - 1] = '\0';
        user_menu_count++;
    }

    f_close(&user_file);
}

static void menu_render_user_menu(void) {
    lcd_clear();
    lcd_print_padded_line(0, "User Menu");

    if (user_menu_count == 0) {
        lcd_print_padded_line(1, "User ID: none");
        lcd_print_padded_line(2, "No assigned users");
        lcd_print_padded_line(3, "*:Back");
        return;
    }

    char line1[21];
    snprintf(line1, sizeof(line1), "User ID: %u %s",
             (unsigned)user_menu_ids[user_menu_index],
             user_menu_names[user_menu_index]);
    lcd_print_padded_line(1, line1);
    lcd_print_padded_line(2, "A/B:Cycle");
    lcd_print_padded_line(3, "#:Select  *:Back");
}

static void user_menu_start(void) {
    user_menu_load_assigned();
    menu_render_user_menu();
}

static void make_username_from_name(const char *name, uint8_t user_id, char *username_out, size_t username_len) {
    if (!username_out || username_len < 4) return;

    size_t out = 0;
    for (size_t i = 0; name && name[i] != '\0' && out + 1 < username_len; i++) {
        char ch = name[i];
        if (isalnum((unsigned char)ch)) {
            username_out[out++] = (char)tolower((unsigned char)ch);
        } else if (ch == ' ' && out + 1 < username_len) {
            username_out[out++] = '_';
        }
    }
    username_out[out] = '\0';

    if (out == 0) {
        snprintf(username_out, username_len, "user%02u", user_id);
    }
}

static void menu_render_add_user(void) {
    lcd_clear();

    char line0[21];
    snprintf(line0, sizeof(line0), "Add New User ID %u", (unsigned)add_user_id);
    lcd_print_padded_line(0, line0);

    char line1[21];
    snprintf(line1, sizeof(line1), "Name: %s", add_user_name);
    lcd_print_padded_line(1, line1);

    char line2[21];
    snprintf(line2, sizeof(line2), "Gender: %s", add_user_gender_male ? "Male" : "Female");
    lcd_print_padded_line(2, line2);

    char line3[21];
    snprintf(line3, sizeof(line3), "Language: %s", add_user_language);
    lcd_print_padded_line(3, line3);
}

static void add_user_start(void) {
    add_user_id = 1;
    if (!user_list_find_first_available(&add_user_id)) {
        add_user_id = USER_ID_MAX;
    }

    memset(add_user_name, 0, sizeof(add_user_name));
    add_user_name[0] = 'A';
    add_user_name[1] = '\0';
    add_user_name_len = 1;
    add_user_cursor = 0;
    add_user_gender_male = true;
    add_user_step = ADD_USER_STEP_NAME;
    add_user_lang_index = LANG_ENGLISH;
    language_name_from_index(add_user_lang_index, add_user_language, sizeof(add_user_language));
    menu_render_add_user();
}

static void menu_render_unrec_select(void) {
    lcd_clear();
    lcd_print_padded_line(0, "Unrec: 1-3 sel");

    for (uint8_t i = 0; i < 3; i++) {
        char line[21];
        if (i < unrec_preview_count) {
            snprintf(line, sizeof(line), "%u:%s", (unsigned)(i + 1), unrec_preview[i]);
        } else {
            snprintf(line, sizeof(line), "%u:<empty>", (unsigned)(i + 1));
        }
        lcd_print_padded_line((uint8_t)(i + 1), line);
    }
}

static void menu_render_input(const char *title) {
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print(title);
    lcd_set_cursor(0, 1);
    lcd_print(input_buffer_line);
    lcd_set_cursor(0, 3);
    lcd_print("#:OK  *:CLR");
}

static void lcd_print_centered_line(uint8_t row, const char *text) {
    char line[21];
    memset(line, ' ', 20);
    line[20] = '\0';

    if (text && text[0] != '\0') {
        size_t len = strnlen(text, 20);
        size_t left = (20 - len) / 2;
        memcpy(&line[left], text, len);
    }

    lcd_set_cursor(0, row);
    lcd_print(line);
}

static bool training_word_capture_exists(const user_profile_t *user, const char *word) {
    if (!user || !user->set || !word || word[0] == '\0') return false;

    char path[160];
    snprintf(path, sizeof(path), "0:/microsd/%s/%s.dat", user->username, word);
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK;
}

static bool load_training_words_from_file(const char *path) {
    FIL file;
    if (f_open(&file, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) return false;

    training_word_count = 0;
    training_word_index = 0;

    char line[96];
    while (f_gets(line, sizeof(line), &file) && training_word_count < TRAIN_WORDS_MAX) {
        size_t l = strcspn(line, "\r\n");
        line[l] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;

        strncpy(training_words[training_word_count], line, DICT_WORD_SIZE);
        training_words[training_word_count][DICT_WORD_SIZE] = '\0';
        training_word_count++;
    }

    f_close(&file);
    return training_word_count > 0;
}

static bool training_words_load_for_current_user(void) {
    if (!current_user.set) return false;

    char lang_name[LANG_NAME_SIZE + 1] = "English";
    if (current_user.language[0] != '\0') {
        strncpy(lang_name, current_user.language, sizeof(lang_name) - 1);
        lang_name[sizeof(lang_name) - 1] = '\0';
    }

    char path[160];
    snprintf(path, sizeof(path), "0:/microsd/TrainingWords_%s.txt", lang_name);
    if (load_training_words_from_file(path)) return true;

    for (size_t i = 0; lang_name[i] != '\0'; i++) {
        if (lang_name[i] == ' ') lang_name[i] = '_';
    }
    snprintf(path, sizeof(path), "0:/microsd/TrainingWords_%s.txt", lang_name);
    if (load_training_words_from_file(path)) return true;

    return load_training_words_from_file("0:/microsd/SampleWords.txt");
}

static void menu_render_training_menu(void) {
    lcd_clear();

    if (!current_user.set) {
        lcd_print_padded_line(0, "Training Memnu");
        lcd_print_padded_line(1, "No user selected");
        lcd_print_padded_line(2, "Select user first");
        lcd_print_padded_line(3, "*:Back");
        return;
    }

    if (!training_words_loaded || training_word_count == 0) {
        lcd_print_padded_line(0, "Training Memnu");
        lcd_print_padded_line(1, "No training words");
        lcd_print_padded_line(2, "Generate list first");
        lcd_print_padded_line(3, "*:Back");
        return;
    }

    char line0[21];
    snprintf(line0, sizeof(line0), "Training Memnu %u/%u",
             (unsigned)(training_word_index + 1),
             (unsigned)training_word_count);
    lcd_print_padded_line(0, line0);

    lcd_print_centered_line(1, training_words[training_word_index]);

    if (training_word_capture_exists(&current_user, training_words[training_word_index])) {
        lcd_print_padded_line(2, "Recording: Present");
    } else {
        lcd_print_padded_line(2, "Recording: Missing");
    }

    if (train_state == TRAIN_WAIT_TRIGGER || train_state == TRAIN_CAPTURE) {
        lcd_print_centered_line(3, "Speak When Ready");
    } else {
        lcd_print_padded_line(3, "A/B:Scroll #:Train");
    }
}

// ==============================
// Dictionary lookup with FatFs
// ==============================
static FATFS fs;
static FIL dict_file;
static FIL newwords_file;
static bool sd_ready = false;
static bool dict_ready = false;
static bool newwords_ready = false;
static uint16_t unrecognised_counter = 0;

static bool ensure_microsd_dir(void);
static bool stage2_load_nn_from_sd(uint8_t addr, const char *path);

static bool ensure_logs_dir(void) {
    if (!ensure_microsd_dir()) return false;
    FRESULT res = f_mkdir("0:/microsd/logs");
    return (res == FR_OK || res == FR_EXIST);
}

static void ann_log_emit(const char *username, const char *line) {
    if (!line || line[0] == '\0') return;

    output_send_line(line);

    if (!sd_ready) return;
    if (!ensure_logs_dir()) return;

    char path[192];
    if (username && username[0] != '\0') {
        snprintf(path, sizeof(path), "0:/microsd/logs/%s_ann_train.log", username);
    } else {
        snprintf(path, sizeof(path), "0:/microsd/logs/ann_train.log");
    }

    FIL log_file;
    FRESULT res = f_open(&log_file, path, FA_WRITE | FA_OPEN_APPEND);
    if (res != FR_OK) {
        res = f_open(&log_file, path, FA_WRITE | FA_CREATE_ALWAYS);
    }
    if (res != FR_OK) return;

    UINT bw = 0;
    size_t len = strnlen(line, 220);
    if (len > 0) {
        f_write(&log_file, line, (UINT)len, &bw);
    }
    const char *crlf = "\r\n";
    f_write(&log_file, crlf, 2, &bw);
    f_close(&log_file);
}

static bool create_language_file(void) {
    FIL lang_file;
    FRESULT res = f_open(&lang_file, "0:/microsd/Language.dat", FA_CREATE_NEW | FA_WRITE);
    
    // If file exists, don't overwrite
    if (res == FR_EXIST) {
        printf("INFO: Language.dat already exists\n");
        return true;
    }
    
    if (res != FR_OK) {
        printf("ERROR: f_open Language.dat failed with code %d\n", res);
        return false;
    }
    
    UINT bw;
    
    // Define initial languages
    struct {
        uint16_t id;
        const char *name;
    } languages[] = {
        {0, "Unknown"},
        {1, "English"},
        {2, "Spanish"},
        {3, "French"},
        {4, "German"},
        {5, "Italian"},
        {6, "Portuguese"},
        {7, "Russian"},
        {8, "Chinese"},
        {9, "Japanese"},
        {10, "Korean"},
        {11, "Arabic"},
        {12, "Hindi"},
        {13, "Dutch"},
        {14, "Swedish"},
        {15, "Turkish"},
        {16, "Polish"},
        {17, "Greek"},
        {18, "Hebrew"},
        {19, "Vietnamese"}
    };
    
    for (int i = 0; i < 20; i++) {
        char line[64];
        int n = snprintf(line, sizeof(line), "%02X %s\r\n", (unsigned)languages[i].id, languages[i].name);

        res = f_write(&lang_file, line, (UINT)n, &bw);
        if (res != FR_OK || bw != (UINT)n) {
            printf("ERROR: writing language record %d failed\n", i);
            f_close(&lang_file);
            return false;
        }
    }
    
    f_close(&lang_file);
    printf("INFO: Language.dat created with 20 languages\n");
    return true;
}

static bool create_user_list_file(void) {
    FIL user_file;
    FRESULT res = f_open(&user_file, "0:/microsd/UserList.txt", FA_CREATE_NEW | FA_WRITE);

    if (res == FR_EXIST) {
        printf("INFO: UserList.txt already exists\n");
        return true;
    }

    if (res != FR_OK) {
        printf("ERROR: f_open UserList.txt failed with code %d\n", res);
        return false;
    }

    UINT bw;
    const char *header = "# id,name\r\n0,Unknown\r\n";
    res = f_write(&user_file, header, (UINT)strlen(header), &bw);
    if (res != FR_OK) {
        f_close(&user_file);
        return false;
    }

    for (uint8_t id = 1; id <= USER_ID_MAX; id++) {
        char line[32];
        int n = snprintf(line, sizeof(line), "%u,User%02u\r\n", id, id);
        res = f_write(&user_file, line, (UINT)n, &bw);
        if (res != FR_OK) {
            f_close(&user_file);
            return false;
        }
    }

    f_close(&user_file);
    printf("INFO: UserList.txt created with IDs 0..%u\n", USER_ID_MAX);
    return true;
}

static bool user_lookup_name(uint8_t user_id, char *name_out, size_t name_out_len) {
    if (!name_out || name_out_len < 2) return false;

    if (user_id == USER_ID_UNKNOWN) {
        strncpy(name_out, "Unknown", name_out_len - 1);
        name_out[name_out_len - 1] = '\0';
        return true;
    }

    FIL user_file;
    FRESULT res = f_open(&user_file, "0:/microsd/UserList.txt", FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK) {
        strncpy(name_out, "Unknown", name_out_len - 1);
        name_out[name_out_len - 1] = '\0';
        return false;
    }

    char line[96];
    bool found = false;

    while (f_gets(line, sizeof(line), &user_file)) {
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;

        char *comma = strchr(line, ',');
        if (!comma) continue;
        *comma = '\0';

        int id = atoi(line);
        if (id != user_id) continue;

        char *name = comma + 1;
        char *eol = strpbrk(name, "\r\n");
        if (eol) *eol = '\0';

        strncpy(name_out, name, name_out_len - 1);
        name_out[name_out_len - 1] = '\0';
        found = true;
        break;
    }

    f_close(&user_file);

    if (!found) {
        strncpy(name_out, "Unknown", name_out_len - 1);
        name_out[name_out_len - 1] = '\0';
    }

    return found;
}

static bool dict_add_unknown_word(const uint8_t *seq) {
    if (!sd_ready) return false;
    
    FIL newwords;
    FRESULT res;
    UINT bw;
    
    // Open NewWords.dat in append mode
    res = f_open(&newwords, "0:/microsd/NewWords.dat", FA_WRITE | FA_OPEN_APPEND);
    if (res != FR_OK) {
        // File doesn't exist, create it
        res = f_open(&newwords, "0:/microsd/NewWords.dat", FA_WRITE | FA_CREATE_NEW);
        if (res != FR_OK) {
            printf("ERROR: failed to create NewWords.dat (code %d)\n", res);
            return false;
        }
    }
    
    // Generate word: "UnRecognisedXX"
    char word[DICT_WORD_SIZE];
    snprintf(word, DICT_WORD_SIZE, "UnRecognised%02d", unrecognised_counter);

    uint8_t language_id = LANG_UNKNOWN;
    if (current_user.set && current_user.language[0] != '\0') {
        language_id = language_id_from_name(current_user.language);
    }

    char record_line[DICT_RECORD_SIZE + 1];
    memset(record_line, ' ', sizeof(record_line));

    for (int i = 0; i < PHONEME_SEQ_LEN; i++) {
        int pos = i * 3;
        snprintf(&record_line[pos], 3, "%02X", seq[i]);
        record_line[pos + 2] = ' ';
    }

    snprintf(&record_line[DICT_LANG_OFFSET], 3, "%02X", language_id);
    record_line[DICT_LANG_OFFSET + DICT_LANG_ID_CHARS] = ' ';

    size_t wlen = strnlen(word, DICT_WORD_SIZE);
    memcpy(&record_line[DICT_WORD_OFFSET], word, wlen);
    record_line[DICT_WORD_OFFSET + DICT_WORD_SIZE] = '\r';
    record_line[DICT_WORD_OFFSET + DICT_WORD_SIZE + 1] = '\n';

    res = f_write(&newwords, record_line, DICT_RECORD_SIZE, &bw);
    f_close(&newwords);
    
    if (res != FR_OK || bw != DICT_RECORD_SIZE) {
        printf("ERROR: failed to write unknown word record\n");
        return false;
    }
    
    printf("INFO: Added unknown word '%s' to NewWords.dat\n", word);
    unrecognised_counter++;
    return true;
}

static bool dict_init(void) {
    FRESULT res = f_mount(&fs, "0:", 1);
    if (res != FR_OK) {
        printf("ERROR: f_mount failed with code %d\n", res);
        return false;
    }

    sd_ready = true;
    if (!ensure_microsd_dir()) {
        printf("ERROR: failed to create microsd directory\n");
        return false;
    }
    
    // Create Language.dat if it doesn't exist
    if (!create_language_file()) {
        printf("WARNING: failed to create Language.dat\n");
    }

    // Create UserList.txt if it doesn't exist
    if (!create_user_list_file()) {
        printf("WARNING: failed to create UserList.txt\n");
    }

    res = f_open(&dict_file, "0:/microsd/Dictionary.dat", FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK) {
        printf("ERROR: f_open Dictionary.dat failed with code %d\n", res);
        return false;
    }

    dict_ready = true;
    return true;
}

static int compare_seq_to_record(const uint8_t *lhs_seq, const uint8_t *rhs_seq) {
    for (int i = 0; i < PHONEME_SEQ_LEN; i++) {
        uint8_t lhs = lhs_seq[i];
        uint8_t rhs = rhs_seq[i];
        if (lhs < rhs) return -1;
        if (lhs > rhs) return 1;
    }
    return 0;
}

static int hex_nibble_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

static bool dict_parse_record_line(const char *record, uint8_t *seq_out, uint8_t *language_id_out, char *word_out, size_t word_out_len) {
    if (!record || !seq_out || !word_out || word_out_len < 2) return false;

    for (int i = 0; i < PHONEME_SEQ_LEN; i++) {
        int pos = i * 3;
        int hi = hex_nibble_to_int(record[pos]);
        int lo = hex_nibble_to_int(record[pos + 1]);
        if (hi < 0 || lo < 0) return false;
        seq_out[i] = (uint8_t)((hi << 4) | lo);
    }

    int lang_hi = hex_nibble_to_int(record[DICT_LANG_OFFSET]);
    int lang_lo = hex_nibble_to_int(record[DICT_LANG_OFFSET + 1]);
    if (lang_hi < 0 || lang_lo < 0) return false;
    if (language_id_out) {
        *language_id_out = (uint8_t)((lang_hi << 4) | lang_lo);
    }

    char raw_word[DICT_WORD_SIZE + 1];
    memcpy(raw_word, &record[DICT_WORD_OFFSET], DICT_WORD_SIZE);
    raw_word[DICT_WORD_SIZE] = '\0';

    for (int i = DICT_WORD_SIZE - 1; i >= 0; i--) {
        if (raw_word[i] == ' ' || raw_word[i] == '\0') raw_word[i] = '\0';
        else break;
    }

    strncpy(word_out, raw_word, word_out_len - 1);
    word_out[word_out_len - 1] = '\0';
    return true;
}

static bool dict_format_record_line(const uint8_t *seq, uint8_t language_id, const char *word, char *record_out) {
    if (!seq || !word || !record_out) return false;

    memset(record_out, ' ', DICT_RECORD_SIZE);

    for (int i = 0; i < PHONEME_SEQ_LEN; i++) {
        int pos = i * 3;
        snprintf(&record_out[pos], 3, "%02X", seq[i]);
        record_out[pos + 2] = ' ';
    }

    snprintf(&record_out[DICT_LANG_OFFSET], 3, "%02X", language_id);
    record_out[DICT_LANG_OFFSET + DICT_LANG_ID_CHARS] = ' ';

    size_t wlen = strnlen(word, DICT_WORD_SIZE);
    memcpy(&record_out[DICT_WORD_OFFSET], word, wlen);
    record_out[DICT_WORD_OFFSET + DICT_WORD_SIZE] = '\r';
    record_out[DICT_WORD_OFFSET + DICT_WORD_SIZE + 1] = '\n';
    return true;
}

static int dict_compare_record_keys(const char *record_a, const char *record_b) {
    uint8_t seq_a[PHONEME_SEQ_LEN] = {0};
    uint8_t seq_b[PHONEME_SEQ_LEN] = {0};
    uint8_t lang_a = LANG_UNKNOWN;
    uint8_t lang_b = LANG_UNKNOWN;
    char word_a[DICT_WORD_SIZE + 1] = {0};
    char word_b[DICT_WORD_SIZE + 1] = {0};

    if (!dict_parse_record_line(record_a, seq_a, &lang_a, word_a, sizeof(word_a))) return 0;
    if (!dict_parse_record_line(record_b, seq_b, &lang_b, word_b, sizeof(word_b))) return 0;

    int seq_cmp = compare_seq_to_record(seq_a, seq_b);
    if (seq_cmp != 0) return seq_cmp;

    if (lang_a < lang_b) return -1;
    if (lang_a > lang_b) return 1;

    return strcasecmp_local(word_a, word_b);
}

static bool dict_read_record_at(FIL *dict, uint32_t index, char *record_out) {
    if (!dict || !record_out) return false;

    FSIZE_t offset = (FSIZE_t)index * (FSIZE_t)DICT_RECORD_SIZE;
    UINT br = 0;
    if (f_lseek(dict, offset) != FR_OK) return false;
    if (f_read(dict, record_out, DICT_RECORD_SIZE, &br) != FR_OK || br < DICT_RECORD_SIZE) return false;
    return true;
}

static bool dict_write_record_at(FIL *dict, uint32_t index, const char *record_in) {
    if (!dict || !record_in) return false;

    FSIZE_t offset = (FSIZE_t)index * (FSIZE_t)DICT_RECORD_SIZE;
    UINT bw = 0;
    if (f_lseek(dict, offset) != FR_OK) return false;
    if (f_write(dict, record_in, DICT_RECORD_SIZE, &bw) != FR_OK || bw != DICT_RECORD_SIZE) return false;
    return true;
}

static bool dict_insert_sorted_record(FIL *dict, const char *record_line) {
    if (!dict || !record_line) return false;

    UINT bw = 0;
    FSIZE_t end = f_size(dict);
    if (f_lseek(dict, end) != FR_OK) return false;
    if (f_write(dict, record_line, DICT_RECORD_SIZE, &bw) != FR_OK || bw != DICT_RECORD_SIZE) return false;

    uint32_t record_count = (uint32_t)(f_size(dict) / DICT_RECORD_SIZE);
    if (record_count == 0) return false;

    uint32_t current_index = record_count - 1;
    char current_record[DICT_RECORD_SIZE + 1] = {0};
    memcpy(current_record, record_line, DICT_RECORD_SIZE);

    char prev_record[DICT_RECORD_SIZE + 1] = {0};

    // Reverse bubble sort pass for the newly appended entry:
    // swap backwards until ordering is correct for binary search.
    while (current_index > 0) {
        uint32_t prev_index = current_index - 1;
        if (!dict_read_record_at(dict, prev_index, prev_record)) return false;

        if (dict_compare_record_keys(prev_record, current_record) <= 0) {
            break;
        }

        if (!dict_write_record_at(dict, prev_index, current_record)) return false;
        if (!dict_write_record_at(dict, current_index, prev_record)) return false;

        current_index = prev_index;
    }

    return true;
}

static bool dict_add_word_with_language(const uint8_t *seq, uint8_t language_id, const char *word) {
    if (!sd_ready || !seq || !word || word[0] == '\0') return false;

    FIL dict;
    FRESULT res = f_open(&dict, "0:/microsd/Dictionary.dat", FA_READ | FA_WRITE | FA_OPEN_EXISTING);
    if (res != FR_OK) return false;

    char record_line[DICT_RECORD_SIZE + 1] = {0};
    if (!dict_format_record_line(seq, language_id, word, record_line)) {
        f_close(&dict);
        return false;
    }

    bool ok = dict_insert_sorted_record(&dict, record_line);
    f_close(&dict);
    return ok;
}

static bool dict_lookup_word(const uint8_t *seq, char *word_out, size_t word_out_len) {
    if (!dict_ready || word_out_len < 2) return false;

    // Record format: 15 hex values (45 chars) + 2-char language ID + space + 26-char word + CRLF
    // Dictionary.dat is sorted by phoneme sequence (binary search possible)
    // NewWords.dat is sequential (linear search required)

    char record[DICT_RECORD_SIZE + 1];
    uint8_t record_seq[PHONEME_SEQ_LEN];
    char record_word[DICT_WORD_SIZE + 1];
    uint8_t record_lang = LANG_UNKNOWN;
    UINT br;
    FRESULT res;
    uint8_t target_lang = LANG_UNKNOWN;
    if (current_user.set && current_user.language[0] != '\0') {
        uint8_t lang_id = language_id_from_name(current_user.language);
        target_lang = lang_id;
    }

    // First, search Dictionary.dat using binary search (Dictionary.dat is sorted).
    uint32_t record_count = (uint32_t)(f_size(&dict_file) / DICT_RECORD_SIZE);
    if (record_count > 0) {
        int32_t low = 0;
        int32_t high = (int32_t)record_count - 1;

        while (low <= high) {
            int32_t mid = low + ((high - low) / 2);
            FSIZE_t offset = (FSIZE_t)mid * (FSIZE_t)DICT_RECORD_SIZE;

            res = f_lseek(&dict_file, offset);
            if (res != FR_OK) break;

            res = f_read(&dict_file, record, DICT_RECORD_SIZE, &br);
            if (res != FR_OK || br < DICT_RECORD_SIZE) break;
            record[DICT_RECORD_SIZE] = '\0';

            if (!dict_parse_record_line(record, record_seq, &record_lang, record_word, sizeof(record_word))) {
                break;
            }

            int cmp = compare_seq_to_record(seq, record_seq);
            if (cmp == 0) {
                if (record_lang == target_lang) {
                    strncpy(word_out, record_word, word_out_len - 1);
                    word_out[word_out_len - 1] = '\0';
                    return true;
                }

                char fallback_word[DICT_WORD_SIZE + 1];
                strncpy(fallback_word, record_word, sizeof(fallback_word) - 1);
                fallback_word[sizeof(fallback_word) - 1] = '\0';

                bool exact_lang_found = false;

                for (int32_t left = mid - 1; left >= low; left--) {
                    FSIZE_t left_offset = (FSIZE_t)left * (FSIZE_t)DICT_RECORD_SIZE;
                    if (f_lseek(&dict_file, left_offset) != FR_OK) break;
                    if (f_read(&dict_file, record, DICT_RECORD_SIZE, &br) != FR_OK || br < DICT_RECORD_SIZE) break;
                    record[DICT_RECORD_SIZE] = '\0';
                    if (!dict_parse_record_line(record, record_seq, &record_lang, record_word, sizeof(record_word))) break;
                    if (compare_seq_to_record(seq, record_seq) != 0) break;
                    if (record_lang == target_lang) {
                        strncpy(word_out, record_word, word_out_len - 1);
                        word_out[word_out_len - 1] = '\0';
                        return true;
                    }
                }

                for (int32_t right = mid + 1; right <= high; right++) {
                    FSIZE_t right_offset = (FSIZE_t)right * (FSIZE_t)DICT_RECORD_SIZE;
                    if (f_lseek(&dict_file, right_offset) != FR_OK) break;
                    if (f_read(&dict_file, record, DICT_RECORD_SIZE, &br) != FR_OK || br < DICT_RECORD_SIZE) break;
                    record[DICT_RECORD_SIZE] = '\0';
                    if (!dict_parse_record_line(record, record_seq, &record_lang, record_word, sizeof(record_word))) break;
                    if (compare_seq_to_record(seq, record_seq) != 0) break;
                    if (record_lang == target_lang) {
                        exact_lang_found = true;
                        strncpy(word_out, record_word, word_out_len - 1);
                        word_out[word_out_len - 1] = '\0';
                        break;
                    }
                }

                if (exact_lang_found) return true;

                strncpy(word_out, fallback_word, word_out_len - 1);
                word_out[word_out_len - 1] = '\0';
                return true;
            }

            if (cmp < 0) {
                high = mid - 1;
            } else {
                low = mid + 1;
            }
        }
    }

    // Not found in Dictionary.dat, try NewWords.dat
    FIL newwords;
    res = f_open(&newwords, "0:/microsd/NewWords.dat", FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK) {
        // NewWords.dat doesn't exist yet, word truly not found
        return false;
    }

    while (1) {
        res = f_read(&newwords, record, DICT_RECORD_SIZE, &br);
        if (res != FR_OK || br < DICT_RECORD_SIZE) break;
        record[DICT_RECORD_SIZE] = '\0';

        if (!dict_parse_record_line(record, record_seq, &record_lang, record_word, sizeof(record_word))) {
            break;
        }

        int match = (compare_seq_to_record(seq, record_seq) == 0) && (record_lang == target_lang);

        if (match) {
            strncpy(word_out, record_word, word_out_len - 1);
            word_out[word_out_len - 1] = '\0';
            f_close(&newwords);
            return true;
        }
    }

    f_close(&newwords);
    return false;
}

// ==============================
// SD helpers (NN data)
// ==============================
static bool ensure_microsd_dir(void) {
    FRESULT res = f_mkdir("0:/microsd");
    if (res == FR_OK || res == FR_EXIST) return true;
    return false;
}

static bool nn_parse_index(const char *name, uint8_t *index_out) {
    const char *prefix = "RecognizerANN";
    size_t len = strlen(name);
    if (len != 19) return false; // RecognizerANNXX.dat
    if (strncmp(name, prefix, 13) != 0) return false;
    if (name[15] != '.' || name[16] != 'd' || name[17] != 'a' || name[18] != 't') return false;
    if (name[13] < '0' || name[13] > '9' || name[14] < '0' || name[14] > '9') return false;
    *index_out = (uint8_t)((name[13] - '0') * 10 + (name[14] - '0'));
    return true;
}

static bool nn_version_from_path(const char *path, uint8_t *version_out) {
    if (!path || !version_out) return false;
    const char *name = strrchr(path, '/');
    name = name ? (name + 1) : path;
    return nn_parse_index(name, version_out);
}

static void ann_version_sort_asc(uint8_t *values, uint8_t count) {
    if (!values || count < 2) return;
    for (uint8_t i = 0; i < count; i++) {
        for (uint8_t j = (uint8_t)(i + 1); j < count; j++) {
            if (values[j] < values[i]) {
                uint8_t t = values[i];
                values[i] = values[j];
                values[j] = t;
            }
        }
    }
}

static bool ann_scan_saved_versions(uint8_t *versions_out, uint8_t *count_out) {
    if (!versions_out || !count_out) return false;
    *count_out = 0;
    if (!ensure_microsd_dir()) return false;

    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, "0:/microsd") != FR_OK) return false;

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        const char *name = fno.fname[0] ? fno.fname : fno.altname;
        uint8_t version = 0;
        if (nn_parse_index(name, &version)) {
            bool exists = false;
            for (uint8_t i = 0; i < *count_out; i++) {
                if (versions_out[i] == version) {
                    exists = true;
                    break;
                }
            }
            if (!exists && *count_out < ANN_VERSION_MAX) {
                versions_out[*count_out] = version;
                (*count_out)++;
            }
        }
    }

    f_closedir(&dir);
    ann_version_sort_asc(versions_out, *count_out);
    return true;
}

static bool ann_path_from_version(uint8_t version, char *path_out, size_t path_len) {
    if (!path_out || path_len == 0 || version > 99) return false;
    snprintf(path_out, path_len, "0:/microsd/RecognizerANN%02u.dat", (unsigned)version);
    return true;
}

static bool load_ann_to_all_stage2(uint8_t version) {
    char path[80];
    if (!ann_path_from_version(version, path, sizeof(path))) return false;

    bool ok = true;
    for (uint8_t i = 0; i < STAGE2_COUNT; i++) {
        uint8_t addr = (uint8_t)(STAGE2_BASE_ADDR + i);
        menu_render_load_ann_progress(version, (uint8_t)(i + 1), STAGE2_COUNT);

        if (!stage2_load_nn_from_sd(addr, path)) {
            ok = false;
        }
    }

    return ok;
}

static bool load_ann_menu_start(void) {
    ann_version_count = 0;
    ann_version_index = 0;

    if (!ann_scan_saved_versions(ann_versions, &ann_version_count)) {
        return false;
    }

    if (ann_version_count > 0) {
        ann_version_index = (uint8_t)(ann_version_count - 1);
    }

    return true;
}

static bool nn_next_filename(char *path_out, size_t path_len) {
    if (!ensure_microsd_dir()) return false;

    DIR dir;
    FILINFO fno;
    uint8_t max_index = 0;
    bool any = false;

    if (f_opendir(&dir, "0:/microsd") != FR_OK) return false;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        const char *name = fno.fname[0] ? fno.fname : fno.altname;
        uint8_t idx = 0;
        if (nn_parse_index(name, &idx)) {
            if (!any || idx > max_index) {
                max_index = idx;
                any = true;
            }
        }
    }
    f_closedir(&dir);

    uint8_t next = any ? (uint8_t)(max_index + 1) : 0;
    if (next > 99) return false;
    snprintf(path_out, path_len, "0:/microsd/RecognizerANN%02u.dat", next);
    return true;
}

static bool user_folder_prepare(const user_profile_t *user) {
    if (!sd_ready || !user || !user->set) return false;
    if (!ensure_microsd_dir()) return false;

    char path[96];
    snprintf(path, sizeof(path), "0:/microsd/%s", user->username);
    FRESULT res = f_mkdir(path);
    if (!(res == FR_OK || res == FR_EXIST)) return false;

    char info_path[128];
    snprintf(info_path, sizeof(info_path), "0:/microsd/%s/UserData.txt", user->username);
    FIL file;
    res = f_open(&file, info_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) return false;

    char line[160];
    int n = snprintf(line, sizeof(line), "Name: %s\r\nAge: %u\r\nGender: %s\r\nLanguage: %s\r\n",
                     user->full_name, user->age, user->gender, user->language);
    UINT bw = 0;
    res = f_write(&file, line, (UINT)n, &bw);
    f_close(&file);
    return (res == FR_OK && bw == (UINT)n);
}

static bool sample_words_exists(void) {
    FILINFO fno;
    return f_stat("0:/microsd/SampleWords.txt", &fno) == FR_OK;
}

static bool word_is_short(const char *word, size_t len) {
    if (len == 0 || len > MAX_WORD_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        if (!isalpha((unsigned char)word[i]) && word[i] != '\'') return false;
    }
    return true;
}

static int phoneme_count(const uint8_t *seq) {
    int count = 0;
    for (int i = 0; i < PHONEME_SEQ_LEN; i++) {
        uint8_t id = seq[i];
        if (id >= 0x05 && id <= 0x2C) count++;
    }
    return count;
}

static bool generate_sample_words(void) {
    if (!sd_ready) return false;
    if (!ensure_microsd_dir()) return false;
    if (sample_words_exists()) return true;

    FIL dict;
    FRESULT res = f_open(&dict, "0:/microsd/Dictionary.dat", FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK) return false;

    FIL out;
    res = f_open(&out, "0:/microsd/SampleWords.txt", FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        f_close(&dict);
        return false;
    }

    const int phoneme_min = 0x05;
    const int phoneme_max = 0x2C;
    const int phoneme_count_total = phoneme_max - phoneme_min + 1;
    uint8_t counts[40] = {0};
    uint8_t target_language_id = LANG_UNKNOWN;
    if (current_user.set && current_user.language[0] != '\0') {
        target_language_id = language_id_from_name(current_user.language);
    }

    bool progress = true;
    while (progress) {
        progress = false;
        uint8_t best_seq[PHONEME_SEQ_LEN] = {0};
        int best_gain = 0;
        char best_word[DICT_WORD_SIZE + 1] = {0};

        f_lseek(&dict, 0);
        UINT br = 0;
        char record[DICT_RECORD_SIZE + 1];
        uint8_t parsed_seq[PHONEME_SEQ_LEN];
        uint8_t parsed_lang = LANG_UNKNOWN;
        char parsed_word[DICT_WORD_SIZE + 1];
        while (1) {
            res = f_read(&dict, record, DICT_RECORD_SIZE, &br);
            if (res != FR_OK || br < DICT_RECORD_SIZE) break;
            record[DICT_RECORD_SIZE] = '\0';

            if (!dict_parse_record_line(record, parsed_seq, &parsed_lang, parsed_word, sizeof(parsed_word))) {
                continue;
            }

            if (parsed_lang != target_language_id) continue;

            int pcount = phoneme_count(parsed_seq);
            if (pcount == 0 || pcount > MAX_PHONEMES_PER_WORD) continue;

            size_t wlen = strnlen(parsed_word, DICT_WORD_SIZE);
            if (!word_is_short(parsed_word, wlen)) continue;

            int gain = 0;
            for (int i = 0; i < PHONEME_SEQ_LEN; i++) {
                uint8_t id = parsed_seq[i];
                if (id >= phoneme_min && id <= phoneme_max) {
                    int idx = id - phoneme_min;
                    if (counts[idx] < 3) gain++;
                }
            }

            if (gain > best_gain) {
                best_gain = gain;
                memcpy(best_seq, parsed_seq, sizeof(best_seq));
                strncpy(best_word, parsed_word, sizeof(best_word) - 1);
            }
        }

        if (best_gain > 0) {
            UINT bw = 0;
            f_write(&out, best_word, (UINT)strlen(best_word), &bw);
            f_write(&out, "\r\n", 2, &bw);

            for (int i = 0; i < PHONEME_SEQ_LEN; i++) {
                uint8_t id = best_seq[i];
                if (id >= phoneme_min && id <= phoneme_max) {
                    int idx = id - phoneme_min;
                    if (counts[idx] < 3) counts[idx]++;
                }
            }
            progress = true;
        }

        bool done = true;
        for (int i = 0; i < phoneme_count_total; i++) {
            if (counts[i] < 3) {
                done = false;
                break;
            }
        }
        if (done) break;
    }

    f_close(&dict);
    f_close(&out);

    return true;
}

static bool dict_merge_new_words(void) {
    if (!sd_ready) {
        printf("ERROR: SD not ready for merge\\n");
        return false;
    }

    // Check if NewWords.dat exists
    FILINFO fno;
    FRESULT res = f_stat("0:/microsd/NewWords.dat", &fno);
    if (res != FR_OK) {
        printf("INFO: No NewWords.dat to merge\\n");
        return true;  // Not an error, just nothing to do
    }

    // Open Dictionary.dat in read/write mode for sorted insertion
    FIL dict;
    res = f_open(&dict, "0:/microsd/Dictionary.dat", FA_READ | FA_WRITE | FA_OPEN_EXISTING);
    if (res != FR_OK) {
        printf("ERROR: Failed to open Dictionary.dat for update (code %d)\\n", res);
        return false;
    }

    // Open NewWords.dat for reading
    FIL newwords;
    res = f_open(&newwords, "0:/microsd/NewWords.dat", FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK) {
        f_close(&dict);
        printf("ERROR: Failed to open NewWords.dat (code %d)\\n", res);
        return false;
    }

    uint8_t record[DICT_RECORD_SIZE];
    UINT br;
    uint32_t merged_count = 0;

    // Insert all NewWords.dat records into Dictionary.dat with reverse-bubble sorted placement
    while (1) {
        res = f_read(&newwords, record, DICT_RECORD_SIZE, &br);
        if (res != FR_OK || br < DICT_RECORD_SIZE) break;

        if (!dict_insert_sorted_record(&dict, (const char *)record)) {
            printf("ERROR: Failed to insert merged record in sorted order\\n");
            f_close(&dict);
            f_close(&newwords);
            return false;
        }
        merged_count++;
    }

    f_close(&dict);
    f_close(&newwords);

    // Delete NewWords.dat after successful merge
    res = f_unlink("0:/microsd/NewWords.dat");
    if (res != FR_OK) {
        printf("WARNING: Failed to delete NewWords.dat after merge (code %d)\\n", res);
        // Not fatal, continue
    }

    printf("INFO: Merged %lu new words into Dictionary.dat\\n", merged_count);
    return true;
}

static uint8_t load_unrecognised_preview(void) {
    unrec_preview_count = 0;

    FIL newwords;
    FRESULT res = f_open(&newwords, "0:/microsd/NewWords.dat", FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK) {
        return 0;
    }

    char record[DICT_RECORD_SIZE + 1];
    uint8_t parsed_seq[PHONEME_SEQ_LEN];
    char parsed_word[DICT_WORD_SIZE + 1];
    UINT br;
    while (unrec_preview_count < UNREC_PREVIEW_COUNT) {
        res = f_read(&newwords, record, DICT_RECORD_SIZE, &br);
        if (res != FR_OK || br < DICT_RECORD_SIZE) {
            break;
        }
        record[DICT_RECORD_SIZE] = '\0';

        if (!dict_parse_record_line(record, parsed_seq, NULL, parsed_word, sizeof(parsed_word))) {
            continue;
        }

        strncpy(unrec_preview[unrec_preview_count], parsed_word,
                sizeof(unrec_preview[unrec_preview_count]) - 1);
        unrec_preview[unrec_preview_count][sizeof(unrec_preview[unrec_preview_count]) - 1] = '\0';
        unrec_preview_count++;
    }

    f_close(&newwords);
    return unrec_preview_count;
}

static bool dict_target_from_word(const char *word, uint8_t *target_out) {
    if (!dict_ready) return false;

    FIL dict;
    if (f_open(&dict, "0:/microsd/Dictionary.dat", FA_READ | FA_OPEN_EXISTING) != FR_OK) return false;

    UINT br = 0;
    char record[DICT_RECORD_SIZE + 1];
    uint8_t parsed_seq[PHONEME_SEQ_LEN];
    char parsed_word[DICT_WORD_SIZE + 1];
    bool found = false;

    while (1) {
        if (f_read(&dict, record, DICT_RECORD_SIZE, &br) != FR_OK || br < DICT_RECORD_SIZE) break;
        record[DICT_RECORD_SIZE] = '\0';

        if (!dict_parse_record_line(record, parsed_seq, NULL, parsed_word, sizeof(parsed_word))) {
            continue;
        }

        if (strcasecmp_local(parsed_word, word) == 0) {
            for (int i = 0; i < PHONEME_SEQ_LEN; i++) {
                uint8_t id = parsed_seq[i];
                if (id >= 0x05 && id <= 0x2C) {
                    *target_out = id;
                    found = true;
                    break;
                }
            }
            break;
        }
    }

    f_close(&dict);
    return found;
}

static bool dict_seq_from_word(const char *word, uint8_t *seq_out) {
    if (!dict_ready || !word || !seq_out) return false;

    FIL dict;
    if (f_open(&dict, "0:/microsd/Dictionary.dat", FA_READ | FA_OPEN_EXISTING) != FR_OK) return false;

    UINT br = 0;
    char record[DICT_RECORD_SIZE + 1];
    uint8_t parsed_seq[PHONEME_SEQ_LEN];
    char parsed_word[DICT_WORD_SIZE + 1];
    bool found = false;

    while (1) {
        if (f_read(&dict, record, DICT_RECORD_SIZE, &br) != FR_OK || br < DICT_RECORD_SIZE) break;
        record[DICT_RECORD_SIZE] = '\0';

        if (!dict_parse_record_line(record, parsed_seq, NULL, parsed_word, sizeof(parsed_word))) {
            continue;
        }

        if (strcasecmp_local(parsed_word, word) == 0) {
            memcpy(seq_out, parsed_seq, PHONEME_SEQ_LEN);
            found = true;
            break;
        }
    }

    f_close(&dict);
    return found;
}

static uint8_t build_expected_phoneme_list(const uint8_t *seq, uint8_t *expected_out, uint8_t expected_max) {
    if (!seq || !expected_out || expected_max == 0) return 0;

    uint8_t count = 0;
    for (uint8_t i = 0; i < PHONEME_SEQ_LEN && count < expected_max; i++) {
        uint8_t id = seq[i];
        if (id >= 0x05 && id <= 0x2C) {
            expected_out[count++] = id;
        }
    }
    return count;
}

static uint8_t sequence_order_match_percent(const uint8_t *expected,
                                            uint8_t expected_count,
                                            const uint8_t *observed,
                                            uint16_t observed_count) {
    if (!expected || expected_count == 0) return 0;
    if (!observed || observed_count == 0) return 0;

    uint8_t matched = 0;
    uint16_t obs_index = 0;

    for (uint8_t exp_index = 0; exp_index < expected_count; exp_index++) {
        uint8_t target = expected[exp_index];
        while (obs_index < observed_count && observed[obs_index] != target) {
            obs_index++;
        }
        if (obs_index >= observed_count) break;
        matched++;
        obs_index++;
    }

    return (uint8_t)((matched * 100u) / expected_count);
}

static bool stage2_write_reg8(uint8_t addr, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_write_blocking(I2C_STAGE2_PORT, addr, buf, 2, false) == 2;
}

static bool stage2_write_reg16(uint8_t addr, uint8_t reg, uint16_t value) {
    uint8_t buf[3] = {reg, (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF)};
    return i2c_write_blocking(I2C_STAGE2_PORT, addr, buf, 3, false) == 3;
}

static bool stage2_read_reg8(uint8_t addr, uint8_t reg, uint8_t *value_out) {
    if (i2c_write_blocking(I2C_STAGE2_PORT, addr, &reg, 1, true) != 1) return false;
    return i2c_read_blocking(I2C_STAGE2_PORT, addr, value_out, 1, false) == 1;
}

static bool stage2_page_read(uint8_t addr, uint8_t page_mode, uint16_t page_addr, uint16_t len, uint8_t *dst) {
    if (!stage2_write_reg8(addr, STAGE2_REG_PAGE_MODE, page_mode)) return false;
    if (!stage2_write_reg16(addr, STAGE2_REG_PAGE_ADDR, page_addr)) return false;
    if (!stage2_write_reg16(addr, STAGE2_REG_PAGE_LEN, len)) return false;

    uint16_t remaining = len;
    while (remaining > 0) {
        uint16_t chunk = remaining > 128 ? 128 : remaining;
        uint8_t reg = STAGE2_REG_PAGE_DATA;
        if (i2c_write_blocking(I2C_STAGE2_PORT, addr, &reg, 1, true) != 1) return false;
        if (i2c_read_blocking(I2C_STAGE2_PORT, addr, dst, chunk, false) != chunk) return false;
        dst += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool stage2_page_write(uint8_t addr, uint8_t page_mode, uint16_t page_addr, uint16_t len, const uint8_t *src) {
    if (!stage2_write_reg8(addr, STAGE2_REG_PAGE_MODE, page_mode)) return false;
    if (!stage2_write_reg16(addr, STAGE2_REG_PAGE_ADDR, page_addr)) return false;
    if (!stage2_write_reg16(addr, STAGE2_REG_PAGE_LEN, len)) return false;

    uint16_t remaining = len;
    while (remaining > 0) {
        uint16_t chunk = remaining > 128 ? 128 : remaining;
        uint8_t buf[129];
        buf[0] = STAGE2_REG_PAGE_DATA;
        memcpy(&buf[1], src, chunk);
        if (i2c_write_blocking(I2C_STAGE2_PORT, addr, buf, (int)(chunk + 1), false) != (int)(chunk + 1)) return false;
        src += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool stage2_clear_input(uint8_t addr) {
    uint8_t zeros[INPUT_NEURONS];
    memset(zeros, 0, sizeof(zeros));
    return stage2_page_write(addr, STAGE2_PAGE_INPUT, 0, INPUT_NEURONS, zeros);
}

static bool stage2_read_input(uint8_t addr, uint8_t *dst) {
    return stage2_page_read(addr, STAGE2_PAGE_INPUT, 0, INPUT_NEURONS, dst);
}

// ==============================
// Training state machine
// ==============================
static char training_active_word[DICT_WORD_SIZE + 1];
static uint8_t capture_buffer[CAPTURE_FRAMES][CAPTURE_FRAME_BYTES];
static uint16_t capture_index = 0;

static uint8_t peak_window[PEAK_WINDOW_FRAMES];
static uint16_t peak_sum = 0;
static uint16_t peak_pos = 0;
static bool speech_started = false;

static void peak_window_reset(void) {
    memset(peak_window, 0, sizeof(peak_window));
    peak_sum = 0;
    peak_pos = 0;
}

static uint8_t compute_peak(const uint8_t *frame) {
    uint8_t peak = 0;
    for (int i = 0; i < CAPTURE_FRAME_BYTES; i++) {
        if (frame[i] > peak) peak = frame[i];
    }
    return peak;
}

static bool save_capture_to_sd(const user_profile_t *user, const char *word, uint16_t frames) {
    if (!user || !user->set) return false;
    if (!user_folder_prepare(user)) return false;
    if (frames == 0 || frames > CAPTURE_FRAMES) return false;

    char path[160];
    snprintf(path, sizeof(path), "0:/microsd/%s/%s.dat", user->username, word);

    FIL file;
    FRESULT res = f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) return false;

    UINT bw = 0;
    uint8_t header[8] = {'C','A','P','0', (uint8_t)CAPTURE_FRAME_BYTES, 0, (uint8_t)frames, 0};
    res = f_write(&file, header, sizeof(header), &bw);
    if (res != FR_OK || bw != sizeof(header)) {
        f_close(&file);
        return false;
    }

    for (uint16_t i = 0; i < frames; i++) {
        res = f_write(&file, capture_buffer[i], CAPTURE_FRAME_BYTES, &bw);
        if (res != FR_OK || bw != CAPTURE_FRAME_BYTES) {
            f_close(&file);
            return false;
        }
    }

    f_close(&file);
    return true;
}

static void training_start(void) {
    if (!sd_ready || !current_user.set) {
        training_words_loaded = false;
        training_word_count = 0;
        training_word_index = 0;
        train_state = TRAIN_IDLE;
        return;
    }

    training_words_loaded = training_words_load_for_current_user();
    if (!training_words_loaded || training_word_count == 0) {
        training_word_count = 0;
        training_word_index = 0;
    }

    train_state = TRAIN_IDLE;
    peak_window_reset();
    capture_index = 0;
    speech_started = false;
    training_active_word[0] = '\0';
}

static void training_stop(void) {
    uint8_t addr = (uint8_t)(STAGE2_BASE_ADDR + TRAIN_BEAM_INDEX);
    stage2_write_reg16(addr, STAGE2_REG_CONTROL, 0x0000);
    stage2_clear_input(addr);

    train_state = TRAIN_IDLE;
    capture_index = 0;
    speech_started = false;
    training_active_word[0] = '\0';
}

static bool training_begin_capture(void) {
    if (!training_words_loaded || training_word_count == 0 || !current_user.set) return false;

    uint8_t addr = (uint8_t)(STAGE2_BASE_ADDR + TRAIN_BEAM_INDEX);
    if (!stage2_clear_input(addr)) return false;

    strncpy(training_active_word, training_words[training_word_index], sizeof(training_active_word) - 1);
    training_active_word[sizeof(training_active_word) - 1] = '\0';

    peak_window_reset();
    capture_index = 0;
    speech_started = false;
    train_state = TRAIN_WAIT_TRIGGER;
    menu_render_training_menu();
    return true;
}

static void training_abort_to_main(void) {
    training_stop();
    menu_state = MENU_MAIN;
    menu_main_page = 1;
    menu_render_main();
}

static void training_tick(void) {
    uint8_t addr = (uint8_t)(STAGE2_BASE_ADDR + TRAIN_BEAM_INDEX);

    static absolute_time_t last_sample = {0};
    int64_t elapsed = absolute_time_diff_us(last_sample, get_absolute_time());
    if (elapsed < (INPUT_PERIOD_MS * 1000)) return;
    last_sample = get_absolute_time();

    uint8_t frame[INPUT_NEURONS];
    if (!stage2_read_input(addr, frame)) return;

    uint8_t peak = compute_peak(frame);
    peak_sum -= peak_window[peak_pos];
    peak_window[peak_pos] = peak;
    peak_sum += peak;
    peak_pos = (uint16_t)((peak_pos + 1) % PEAK_WINDOW_FRAMES);

    uint8_t avg_peak = (uint8_t)(peak_sum / PEAK_WINDOW_FRAMES);

    switch (train_state) {
        case TRAIN_IDLE:
            break;
        case TRAIN_WAIT_TRIGGER:
            if (peak > avg_peak) {
                speech_started = true;
                if (capture_index < CAPTURE_FRAMES) {
                    memcpy(capture_buffer[capture_index], frame, CAPTURE_FRAME_BYTES);
                    capture_index++;
                }
                train_state = TRAIN_CAPTURE;
            }
            break;
        case TRAIN_CAPTURE:
            if (capture_index < CAPTURE_FRAMES) {
                memcpy(capture_buffer[capture_index], frame, CAPTURE_FRAME_BYTES);
                capture_index++;
            }

            bool spoken_done = (speech_started && capture_index >= TRAIN_MIN_SPOKEN_FRAMES && peak <= avg_peak);
            if (spoken_done || capture_index >= CAPTURE_FRAMES) {
                stage2_write_reg16(addr, STAGE2_REG_CONTROL, STAGE2_CTRL_FREEZE_PAUSE);
                train_state = TRAIN_SAVE;
            }
            break;
        case TRAIN_SAVE:
            if (save_capture_to_sd(&current_user, training_active_word, capture_index)) {
                stage2_clear_input(addr);
                stage2_write_reg16(addr, STAGE2_REG_CONTROL, 0x0000);
                train_state = TRAIN_IDLE;
                menu_render_training_menu();
            } else {
                stage2_clear_input(addr);
                stage2_write_reg16(addr, STAGE2_REG_CONTROL, 0x0000);
                train_state = TRAIN_IDLE;
                menu_render_training_menu();
            }
            break;
        default:
            break;
    }
}

static bool stage2_save_nn_to_sd(uint8_t addr, char *path_out, size_t path_len, bool show_progress) {
    if (!sd_ready) return false;
    if (!nn_next_filename(path_out, path_len)) return false;

    uint8_t version = 0;
    nn_version_from_path(path_out, &version);
    if (show_progress) menu_render_save_ann_progress(version, "Preparing", 0);

    if (!stage2_write_reg16(addr, STAGE2_REG_CONTROL, STAGE2_CTRL_FREEZE_PAUSE)) return false;
    sleep_ms(5);

    uint8_t buffer[NN_TOTAL_SIZE];
    uint8_t *ptr = buffer;

    if (show_progress) menu_render_save_ann_progress(version, "Read W1", 20);
    if (!stage2_page_read(addr, STAGE2_PAGE_W1, 0, W1_SIZE, ptr)) goto cleanup;
    ptr += W1_SIZE;
    if (show_progress) menu_render_save_ann_progress(version, "Read B1", 40);
    if (!stage2_page_read(addr, STAGE2_PAGE_B1, 0, B1_SIZE, ptr)) goto cleanup;
    ptr += B1_SIZE;
    if (show_progress) menu_render_save_ann_progress(version, "Read W2", 60);
    if (!stage2_page_read(addr, STAGE2_PAGE_W2, 0, W2_SIZE, ptr)) goto cleanup;
    ptr += W2_SIZE;
    if (show_progress) menu_render_save_ann_progress(version, "Read B2", 80);
    if (!stage2_page_read(addr, STAGE2_PAGE_B2, 0, B2_SIZE, ptr)) goto cleanup;

    FIL file;
    FRESULT res = f_open(&file, path_out, FA_WRITE | FA_CREATE_NEW);
    if (res != FR_OK) goto cleanup;

    UINT bw = 0;
    uint8_t header[16] = {'N','N','D','T', 0x01, 0x00, 0x00, 0x00,
                          (uint8_t)(INPUT_NEURONS & 0xFF), (uint8_t)(INPUT_NEURONS >> 8),
                          (uint8_t)(HIDDEN_NEURONS & 0xFF), (uint8_t)(HIDDEN_NEURONS >> 8),
                          (uint8_t)(OUTPUT_NEURONS & 0xFF), (uint8_t)(OUTPUT_NEURONS >> 8),
                          0x00, 0x00};
    res = f_write(&file, header, sizeof(header), &bw);
    if (res != FR_OK || bw != sizeof(header)) {
        f_close(&file);
        goto cleanup;
    }

    res = f_write(&file, buffer, NN_TOTAL_SIZE, &bw);
    f_close(&file);
    if (res != FR_OK || bw != NN_TOTAL_SIZE) goto cleanup;

    if (show_progress) menu_render_save_ann_progress(version, "Saved", 100);
    stage2_write_reg16(addr, STAGE2_REG_CONTROL, 0x0000);
    return true;

cleanup:
    stage2_write_reg16(addr, STAGE2_REG_CONTROL, 0x0000);
    return false;
}

static bool stage2_load_nn_from_sd(uint8_t addr, const char *path) {
    if (!sd_ready) return false;

    FIL file;
    FRESULT res = f_open(&file, path, FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK) return false;

    uint8_t header[16];
    UINT br = 0;
    res = f_read(&file, header, sizeof(header), &br);
    if (res != FR_OK || br != sizeof(header)) {
        f_close(&file);
        return false;
    }
    if (memcmp(header, "NNDT", 4) != 0) {
        f_close(&file);
        return false;
    }

    uint16_t in_n = (uint16_t)(header[8] | (header[9] << 8));
    uint16_t hid_n = (uint16_t)(header[10] | (header[11] << 8));
    uint16_t out_n = (uint16_t)(header[12] | (header[13] << 8));
    if (in_n != INPUT_NEURONS || hid_n != HIDDEN_NEURONS || out_n != OUTPUT_NEURONS) {
        f_close(&file);
        return false;
    }

    uint8_t buffer[NN_TOTAL_SIZE];
    res = f_read(&file, buffer, NN_TOTAL_SIZE, &br);
    f_close(&file);
    if (res != FR_OK || br != NN_TOTAL_SIZE) return false;

    if (!stage2_write_reg16(addr, STAGE2_REG_CONTROL, STAGE2_CTRL_FREEZE_PAUSE)) return false;
    sleep_ms(5);

    const uint8_t *ptr = buffer;
    bool ok = stage2_page_write(addr, STAGE2_PAGE_W1, 0, W1_SIZE, ptr);
    ptr += W1_SIZE;
    ok = ok && stage2_page_write(addr, STAGE2_PAGE_B1, 0, B1_SIZE, ptr);
    ptr += B1_SIZE;
    ok = ok && stage2_page_write(addr, STAGE2_PAGE_W2, 0, W2_SIZE, ptr);
    ptr += W2_SIZE;
    ok = ok && stage2_page_write(addr, STAGE2_PAGE_B2, 0, B2_SIZE, ptr);

    stage2_write_reg16(addr, STAGE2_REG_CONTROL, 0x0000);
    return ok;
}

// ==============================
// I2C helpers
// ==============================
static bool stage2_read_fifo_len(uint8_t addr, uint16_t *len_out) {
    uint8_t reg = STAGE2_REG_FIFO_LEN;
    uint8_t buf[2] = {0};
    int w = i2c_write_blocking(I2C_STAGE2_PORT, addr, &reg, 1, true);
    if (w != 1) return false;
    int r = i2c_read_blocking(I2C_STAGE2_PORT, addr, buf, 2, false);
    if (r != 2) return false;
    *len_out = (uint16_t)(buf[0] | (buf[1] << 8));
    return true;
}

static bool stage2_read_fifo_entry(uint8_t addr, stage2_entry_t *entry) {
    uint8_t reg = STAGE2_REG_FIFO_READ;
    uint8_t buf[5] = {0};
    int w = i2c_write_blocking(I2C_STAGE2_PORT, addr, &reg, 1, true);
    if (w != 1) return false;
    int r = i2c_read_blocking(I2C_STAGE2_PORT, addr, buf, 5, false);
    if (r != 5) return false;
    entry->max_id = buf[0];
    entry->max_val = buf[1];
    entry->female_val = buf[2];
    entry->male_val = buf[3];
    entry->user_id = buf[4];
    return true;
}

// ==============================
// Placeholder dictionary + translation
// ==============================
static void handle_stage2_entry(uint8_t beam_idx, const stage2_entry_t *entry) {
    beam_seq_t *seq = &beam_sequences[beam_idx];

    char user_name[32];
    user_lookup_name(entry->user_id, user_name, sizeof(user_name));

    // Append phoneme id to sequence (shift if full)
    if (seq->count < PHONEME_SEQ_LEN) {
        seq->seq[seq->count++] = entry->max_id;
    } else {
        memmove(&seq->seq[0], &seq->seq[1], PHONEME_SEQ_LEN - 1);
        seq->seq[PHONEME_SEQ_LEN - 1] = entry->max_id;
    }

    bool silence = (entry->max_id == SIL_WORD_ID) || (entry->max_id == SIL_SENTENCE_ID);
    if (!silence || seq->count < PHONEME_SEQ_LEN) {
        return;
    }

    char word[DICT_WORD_SIZE + 1];
    if (dict_lookup_word(seq->seq, word, sizeof(word))) {
        const char *gender = (entry->female_val >= entry->male_val) ? "female" : "male";
        uint8_t conf = (entry->female_val >= entry->male_val) ? entry->female_val : entry->male_val;
        char line[160];
        snprintf(line, sizeof(line), "beam=%u user_id=%u user=%s word=%s gender=%s conf=%u",
                 beam_idx, entry->user_id, user_name, word, gender, conf);
        output_send_line(line);
        if (beam_idx == TRAIN_BEAM_INDEX) {
            word_history_push(word);
        }
        seq->count = 0; // reset after a match
    } else {
        // Word not found - add to dictionary with language ID 0 (unknown)
        if (dict_add_unknown_word(seq->seq)) {
            // Successfully added - output the new unrecognised word
            char unrec_word[DICT_WORD_SIZE + 1];
            snprintf(unrec_word, sizeof(unrec_word), "UnRecognised%02d", unrecognised_counter - 1);
            const char *gender = (entry->female_val >= entry->male_val) ? "female" : "male";
            uint8_t conf = (entry->female_val >= entry->male_val) ? entry->female_val : entry->male_val;
            char line[160];
            snprintf(line, sizeof(line), "beam=%u user_id=%u user=%s word=%s gender=%s conf=%u [NEW]",
                     beam_idx, entry->user_id, user_name, unrec_word, gender, conf);
            output_send_line(line);
            if (beam_idx == TRAIN_BEAM_INDEX) {
                word_history_push(unrec_word);
            }
        }
        seq->count = 0; // reset regardless
    }
}

// ==============================
// Init
// ==============================
static void init_mode_pins(void) {
    gpio_init(MODE_SEL0_GPIO);
    gpio_init(MODE_SEL1_GPIO);
    gpio_set_dir(MODE_SEL0_GPIO, GPIO_IN);
    gpio_set_dir(MODE_SEL1_GPIO, GPIO_IN);
    gpio_pull_up(MODE_SEL0_GPIO);
    gpio_pull_up(MODE_SEL1_GPIO);
}

static void init_word_ready_pins(void) {
    for (int i = 0; i < STAGE2_COUNT; i++) {
        gpio_init(word_ready_pins[i]);
        gpio_set_dir(word_ready_pins[i], GPIO_IN);
        gpio_pull_up(word_ready_pins[i]);
    }
}

static void init_fault_pins(void) {
    for (int i = 0; i < STAGE2_COUNT; i++) {
        gpio_init(stage2_fault_pins[i]);
        gpio_set_dir(stage2_fault_pins[i], GPIO_OUT);
        gpio_put(stage2_fault_pins[i], 0);
    }
}

static void init_i2c_stage2(void) {
    i2c_init(I2C_STAGE2_PORT, I2C_STAGE2_BAUD);
    gpio_set_function(I2C_STAGE2_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_STAGE2_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_STAGE2_SDA);
    gpio_pull_up(I2C_STAGE2_SCL);
}

static void init_spi_sd(void) {
    spi_init(SD_SPI_PORT, 10 * 1000 * 1000);
    gpio_set_function(SD_SPI_SCK, GPIO_FUNC_SPI);
    gpio_set_function(SD_SPI_TX, GPIO_FUNC_SPI);
    gpio_set_function(SD_SPI_RX, GPIO_FUNC_SPI);
    gpio_init(SD_SPI_CS);
    gpio_set_dir(SD_SPI_CS, GPIO_OUT);
    gpio_put(SD_SPI_CS, 1);
}

static void init_uart_ttl(void) {
    uart_init(TTL_UART, TTL_BAUD);
    gpio_set_function(TTL_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(TTL_UART_RX, GPIO_FUNC_UART);
}

// ==============================
// Command input (USB/TTL)
// ==============================
static bool read_line(char *buf, size_t len) {
    size_t idx = 0;
    while (idx < len - 1) {
        int ch = getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT) break;
        if (ch == '\r') continue;
        if (ch == '\n') {
            buf[idx] = '\0';
            return idx > 0;
        }
        buf[idx++] = (char)ch;
    }
    buf[idx] = '\0';
    return false;
}

static void handle_command(const char *line) {
    if (strncmp(line, "USER ", 5) == 0) {
        char temp[160];
        strncpy(temp, line + 5, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';

        char *username = strtok(temp, " ");
        char *age_str = strtok(NULL, " ");
        char *gender = strtok(NULL, " ");
        char *fullname = strtok(NULL, "");

        if (username && age_str && gender && fullname) {
            strncpy(current_user.username, username, sizeof(current_user.username) - 1);
            current_user.username[sizeof(current_user.username) - 1] = '\0';
            strncpy(current_user.gender, gender, sizeof(current_user.gender) - 1);
            current_user.gender[sizeof(current_user.gender) - 1] = '\0';
            strncpy(current_user.full_name, fullname, sizeof(current_user.full_name) - 1);
            current_user.full_name[sizeof(current_user.full_name) - 1] = '\0';
            current_user.user_id = 0;
            current_user.age = (uint8_t)atoi(age_str);
            current_user.set = true;

            if (user_folder_prepare(&current_user)) {
                output_send_line("User profile saved");
            } else {
                output_send_line("ERROR: Failed to save user profile");
            }
        } else {
            output_send_line("ERROR: USER <username> <age> <gender> <full name>");
        }
    } else if (strcmp(line, "TRAIN") == 0) {
        training_start();
        output_send_line("Training started");
    } else if (strcmp(line, "STOP") == 0) {
        training_stop();
        output_send_line("Training stopped");
    } else if (strcmp(line, "SAMPLEGEN") == 0) {
        if (generate_sample_words()) {
            output_send_line("SampleWords.txt generated");
        } else {
            output_send_line("ERROR: SampleWords generation failed");
        }
    }
}

// ==============================
// Menu and backprop training
// ==============================
static void input_reset(void) {
    memset(input_buffer_line, 0, sizeof(input_buffer_line));
    input_len = 0;
}

static void input_append(char c) {
    if (input_len + 1 < sizeof(input_buffer_line)) {
        input_buffer_line[input_len++] = c;
        input_buffer_line[input_len] = '\0';
    }
}

static bool stage2_set_target(uint8_t addr, uint8_t neuron) {
    return stage2_write_reg8(addr, STAGE2_REG_TARGET_NEURON, neuron);
}

static bool stage2_trigger_backprop(uint8_t addr) {
    return stage2_write_reg16(addr, STAGE2_REG_CONTROL, STAGE2_CTRL_BACKPROP);
}

static bool stage2_read_training_metrics(uint8_t addr,
                                         uint8_t *max_id_out,
                                         uint8_t *max_val_out,
                                         uint8_t *target_val_out,
                                         uint8_t *user_id_out,
                                         uint8_t *user_val_out,
                                         uint8_t *female_val_out,
                                         uint8_t *male_val_out) {
    uint8_t max_id = 0;
    uint8_t max_val = 0;
    uint8_t target_val = 0;
    uint8_t user_id = 0;
    uint8_t user_val = 0;
    uint8_t female_val = 0;
    uint8_t male_val = 0;

    if (!stage2_read_reg8(addr, STAGE2_REG_LAST_MAX_ID, &max_id)) return false;
    if (!stage2_read_reg8(addr, STAGE2_REG_LAST_MAX_VAL, &max_val)) return false;
    if (!stage2_read_reg8(addr, STAGE2_REG_LAST_TARGET_VAL, &target_val)) return false;
    if (!stage2_read_reg8(addr, STAGE2_REG_LAST_USER_ID, &user_id)) return false;
    if (!stage2_read_reg8(addr, STAGE2_REG_LAST_USER_VAL, &user_val)) return false;
    if (!stage2_read_reg8(addr, STAGE2_REG_LAST_FEMALE_VAL, &female_val)) return false;
    if (!stage2_read_reg8(addr, STAGE2_REG_LAST_MALE_VAL, &male_val)) return false;

    if (max_id_out) *max_id_out = max_id;
    if (max_val_out) *max_val_out = max_val;
    if (target_val_out) *target_val_out = target_val;
    if (user_id_out) *user_id_out = user_id;
    if (user_val_out) *user_val_out = user_val;
    if (female_val_out) *female_val_out = female_val;
    if (male_val_out) *male_val_out = male_val;
    return true;
}

static bool i2c_read_stream_reg(uint8_t addr, uint8_t reg, uint8_t *dst, uint16_t len) {
    if (!dst || len == 0) return false;
    if (i2c_write_blocking(I2C_STAGE2_PORT, addr, &reg, 1, true) != 1) return false;
    return i2c_read_blocking(I2C_STAGE2_PORT, addr, dst, len, false) == len;
}

static bool stage4_set_image_line_ptr(uint16_t line) {
    if (line >= STAGE4_IMAGE_LINES) line = 0;
    return stage2_write_reg16(STAGE4_ADDR, STAGE4_REG_IMAGE_LINE_PTR, line);
}

static bool stage4_read_image_line(uint16_t line, uint8_t *line_out) {
    if (!line_out) return false;
    if (!stage4_set_image_line_ptr(line)) return false;
    return i2c_read_stream_reg(STAGE4_ADDR, STAGE4_REG_IMAGE_DATA, line_out, STAGE4_IMAGE_BINS);
}

static bool stage4_generate_image(uint8_t phoneme_id) {
    if (!stage2_write_reg8(STAGE4_ADDR, STAGE4_REG_GEN_PHONEME, phoneme_id)) return false;
    if (!stage2_write_reg8(STAGE4_ADDR, STAGE4_REG_TRAIN_TARGET, phoneme_id)) return false;
    return stage2_write_reg8(STAGE4_ADDR,
                             STAGE4_REG_GEN_COMMAND,
                             (uint8_t)(STAGE4_CMD_RESET_IMAGE_PTR | STAGE4_CMD_GENERATE_IMAGE));
}

static bool stage4_backprop_step(uint8_t phoneme_id, uint8_t feedback_score) {
    if (!stage2_write_reg8(STAGE4_ADDR, STAGE4_REG_TRAIN_TARGET, phoneme_id)) return false;
    if (!stage2_write_reg8(STAGE4_ADDR, STAGE4_REG_TRAIN_FEEDBACK, feedback_score)) return false;
    return stage2_write_reg8(STAGE4_ADDR, STAGE4_REG_GEN_COMMAND, STAGE4_CMD_BACKPROP_STEP);
}

static bool stage4_capture_image(uint8_t image[STAGE4_IMAGE_LINES][STAGE4_IMAGE_BINS]) {
    for (uint16_t line = 0; line < STAGE4_IMAGE_LINES; line++) {
        if (!stage4_read_image_line(line, image[line])) return false;
    }
    return true;
}

static bool stage2_score_generated_image(uint8_t addr,
                                         uint8_t target_id,
                                         uint8_t image[STAGE4_IMAGE_LINES][STAGE4_IMAGE_BINS],
                                         uint8_t *best_target_val_out,
                                         uint8_t *best_max_id_out) {
    uint8_t best_target_val = 0;
    uint8_t best_max_id = 0;
    uint8_t nn_frame[INPUT_NEURONS] = {0};

    if (!stage2_set_target(addr, target_id)) return false;

    for (uint16_t line = 0; line < STAGE4_IMAGE_LINES; line++) {
        memcpy(nn_frame, image[line], STAGE4_IMAGE_BINS);
        nn_frame[STAGE4_IMAGE_BINS] = 0;

        if (!stage2_page_write(addr, STAGE2_PAGE_INPUT, 0, INPUT_NEURONS, nn_frame)) return false;
        sleep_ms(2);

        uint8_t max_id = 0;
        uint8_t max_val = 0;
        uint8_t target_val = 0;
        uint8_t user_id = 0;
        uint8_t user_val = 0;
        uint8_t female_val = 0;
        uint8_t male_val = 0;
        if (!stage2_read_training_metrics(addr,
                                          &max_id,
                                          &max_val,
                                          &target_val,
                                          &user_id,
                                          &user_val,
                                          &female_val,
                                          &male_val)) {
            continue;
        }

        if (target_val > best_target_val) {
            best_target_val = target_val;
            best_max_id = max_id;
        }
    }

    if (best_target_val_out) *best_target_val_out = best_target_val;
    if (best_max_id_out) *best_max_id_out = best_max_id;
    return true;
}

static bool run_speech_generator_training(void) {
    uint8_t stage2_addr = (uint8_t)(STAGE2_BASE_ADDR + TRAIN_BEAM_INDEX);
    uint8_t image[STAGE4_IMAGE_LINES][STAGE4_IMAGE_BINS];

    bool freeze_ok = stage2_write_reg16(stage2_addr, STAGE2_REG_CONTROL, 0x0002);
    if (!freeze_ok) {
        lcd_set_status("Status: SG freeze err");
        return false;
    }

    bool overall_ok = true;
    for (uint8_t phoneme = 0x05; phoneme <= 0x2C; phoneme++) {
        uint8_t best_target = 0;
        uint8_t best_id = 0;
        bool phoneme_ok = false;

        for (uint8_t epoch = 0; epoch < STAGE4_TRAIN_MAX_EPOCHS; epoch++) {
            lcd_clear();
            lcd_print_padded_line(0, "SpeechGen Train");
            char line1[21];
            snprintf(line1, sizeof(line1), "Phoneme:0x%02X", (unsigned)phoneme);
            lcd_print_padded_line(1, line1);
            char line2[21];
            snprintf(line2, sizeof(line2), "Epoch:%u/%u", (unsigned)(epoch + 1), (unsigned)STAGE4_TRAIN_MAX_EPOCHS);
            lcd_print_padded_line(2, line2);
            lcd_print_padded_line(3, "Gen->Eval->Adjust");

            if (!stage4_generate_image(phoneme)) {
                overall_ok = false;
                break;
            }

            if (!stage4_capture_image(image)) {
                overall_ok = false;
                break;
            }

            uint8_t target_val = 0;
            uint8_t max_id = 0;
            if (!stage2_score_generated_image(stage2_addr, phoneme, image, &target_val, &max_id)) {
                overall_ok = false;
                break;
            }

            if (target_val > best_target) {
                best_target = target_val;
                best_id = max_id;
            }

            uint8_t target_pct = (uint8_t)((target_val * 100u) / 255u);
            if (target_pct >= 80) {
                phoneme_ok = true;
                break;
            }

            if (!stage4_backprop_step(phoneme, target_val)) {
                overall_ok = false;
                break;
            }
            sleep_ms(5);
        }

        char log_line[160];
        snprintf(log_line,
                 sizeof(log_line),
                 "SGTRAIN phoneme=0x%02X result=%s target=%u%% max_id=0x%02X",
                 (unsigned)phoneme,
                 phoneme_ok ? "PASS" : "FAIL",
                 (unsigned)((best_target * 100u) / 255u),
                 (unsigned)best_id);
        ann_log_emit(current_user.username, log_line);

        if (!phoneme_ok) {
            overall_ok = false;
        }
    }

    stage2_write_reg16(stage2_addr, STAGE2_REG_CONTROL, 0x0000);
    return overall_ok;
}

static bool run_backprop_on_file(uint8_t addr,
                                 const char *path,
                                 const char *word_label,
                                 const char *log_username,
                                 uint8_t target_id,
                                 const uint8_t *expected_seq,
                                 uint8_t expected_seq_count,
                                 uint8_t expected_user_id,
                                 bool expected_gender_male,
                                 uint8_t *best_target_conf_out,
                                 uint8_t *best_phoneme_order_out,
                                 bool *gender_pass_out,
                                 bool *user_pass_out,
                                 uint8_t *last_max_id_out,
                                 uint8_t *last_user_id_out,
                                 uint8_t *epochs_used_out) {
    FIL file;
    if (f_open(&file, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) return false;

    uint8_t header[8];
    UINT br = 0;
    if (f_read(&file, header, sizeof(header), &br) != FR_OK || br != sizeof(header)) {
        f_close(&file);
        return false;
    }
    if (memcmp(header, "CAP0", 4) != 0 || header[4] != CAPTURE_FRAME_BYTES) {
        f_close(&file);
        return false;
    }

    uint8_t frames = header[6];
    if (frames == 0) {
        f_close(&file);
        return false;
    }

    uint8_t captured_frame[CAPTURE_FRAME_BYTES];
    uint8_t nn_frame[INPUT_NEURONS];
    uint8_t best_target_conf = 0;
    uint8_t best_phoneme_order = 0;
    uint8_t last_max_id = 0;
    uint8_t last_user_id = 0;
    uint8_t epochs_used = 0;
    bool gender_pass = false;
    bool user_pass = false;

    uint8_t expected_local[PHONEME_SEQ_LEN] = {0};
    if (expected_seq && expected_seq_count > 0) {
        if (expected_seq_count > PHONEME_SEQ_LEN) expected_seq_count = PHONEME_SEQ_LEN;
        memcpy(expected_local, expected_seq, expected_seq_count);
    }

    for (uint8_t epoch = 0; epoch < STAGE2_ANN_MAX_EPOCHS; epoch++) {
        if (f_lseek(&file, sizeof(header)) != FR_OK) {
            f_close(&file);
            return false;
        }

        uint8_t epoch_best_target_conf = 0;
        uint8_t epoch_best_gender_val = 0;
        uint8_t epoch_best_user_val = 0;
        uint8_t observed_seq[CAPTURE_FRAMES] = {0};
        uint16_t observed_count = 0;
        uint8_t last_observed_id = 0;

        for (uint8_t i = 0; i < frames; i++) {
            if (f_read(&file, captured_frame, CAPTURE_FRAME_BYTES, &br) != FR_OK || br != CAPTURE_FRAME_BYTES) {
                f_close(&file);
                return false;
            }

            memcpy(nn_frame, captured_frame, CAPTURE_FRAME_BYTES);
            nn_frame[CAPTURE_FRAME_BYTES] = 0;

            if (!stage2_page_write(addr, STAGE2_PAGE_INPUT, 0, INPUT_NEURONS, nn_frame)) {
                f_close(&file);
                return false;
            }
            if (!stage2_set_target(addr, target_id)) {
                f_close(&file);
                return false;
            }
            if (!stage2_trigger_backprop(addr)) {
                f_close(&file);
                return false;
            }

            sleep_ms(5);

            uint8_t max_id = 0;
            uint8_t max_val = 0;
            uint8_t target_val = 0;
            uint8_t user_id = 0;
            uint8_t user_val = 0;
            uint8_t female_val = 0;
            uint8_t male_val = 0;
            if (stage2_read_training_metrics(addr,
                                             &max_id,
                                             &max_val,
                                             &target_val,
                                             &user_id,
                                             &user_val,
                                             &female_val,
                                             &male_val)) {
                if (target_val > epoch_best_target_conf) {
                    epoch_best_target_conf = target_val;
                }
                uint8_t gender_val = expected_gender_male ? male_val : female_val;
                if (gender_val > epoch_best_gender_val) {
                    epoch_best_gender_val = gender_val;
                }
                if (user_val > epoch_best_user_val) {
                    epoch_best_user_val = user_val;
                }

                if (max_id >= 0x05 && max_id <= 0x2C) {
                    if (observed_count == 0 || max_id != last_observed_id) {
                        observed_seq[observed_count++] = max_id;
                        last_observed_id = max_id;
                    }
                }

                last_max_id = max_id;
                last_user_id = user_id;
            }
        }

        uint8_t epoch_phoneme_order = sequence_order_match_percent(expected_local,
                                                                   expected_seq_count,
                                                                   observed_seq,
                                                                   observed_count);

        bool epoch_gender_ok = (epoch_best_gender_val >= STAGE2_CERTAINTY_THRESHOLD);
        bool epoch_user_ok = (expected_user_id == 0)
                             ? true
                             : ((last_user_id == expected_user_id) &&
                                (epoch_best_user_val >= STAGE2_CERTAINTY_THRESHOLD));

        char dbg_line[160];
        snprintf(dbg_line,
                 sizeof(dbg_line),
                 "ANNTRAIN word=%s epoch=%u target=%u%% phon=%u%% g=%c u=%c max_id=0x%02X user=%u",
                 (word_label && word_label[0] != '\0') ? word_label : "<unknown>",
                 (unsigned)(epoch + 1),
                 (unsigned)((epoch_best_target_conf * 100u) / 255u),
                 (unsigned)epoch_phoneme_order,
                 epoch_gender_ok ? 'Y' : 'N',
                 epoch_user_ok ? 'Y' : 'N',
                 (unsigned)last_max_id,
                 (unsigned)last_user_id);
        ann_log_emit(log_username, dbg_line);

        epochs_used = (uint8_t)(epoch + 1);
        if (epoch_best_target_conf > best_target_conf) {
            best_target_conf = epoch_best_target_conf;
        }
        if (epoch_phoneme_order > best_phoneme_order) {
            best_phoneme_order = epoch_phoneme_order;
        }
        if (epoch_gender_ok) {
            gender_pass = true;
        }
        if (epoch_user_ok) {
            user_pass = true;
        }

        if (epoch_best_target_conf >= STAGE2_CERTAINTY_THRESHOLD &&
            epoch_phoneme_order >= 80 &&
            epoch_gender_ok &&
            epoch_user_ok) {
            break;
        }
    }

    f_close(&file);

    if (best_target_conf_out) *best_target_conf_out = best_target_conf;
    if (best_phoneme_order_out) *best_phoneme_order_out = best_phoneme_order;
    if (gender_pass_out) *gender_pass_out = gender_pass;
    if (user_pass_out) *user_pass_out = user_pass;
    if (last_max_id_out) *last_max_id_out = last_max_id;
    if (last_user_id_out) *last_user_id_out = last_user_id;
    if (epochs_used_out) *epochs_used_out = epochs_used;

    return (best_target_conf >= STAGE2_CERTAINTY_THRESHOLD) &&
           (best_phoneme_order >= 80) &&
           gender_pass &&
           user_pass;
}

static bool run_backprop_training(void) {
    if (!sd_ready || !current_user.set) return false;

    char user_path[128];
    snprintf(user_path, sizeof(user_path), "0:/microsd/%s", current_user.username);

    DIR udir;
    FILINFO ufno;
    if (f_opendir(&udir, user_path) != FR_OK) return false;

    uint8_t addr = (uint8_t)(STAGE2_BASE_ADDR + TRAIN_BEAM_INDEX);
    bool ok = true;
    uint16_t trained_count = 0;
    uint16_t passed_count = 0;
    uint16_t failed_count = 0;
    char last_result[21] = "Last: none";

    // Freeze incoming stage-1 data while ANN training runs
    if (!stage2_write_reg16(addr, STAGE2_REG_CONTROL, 0x0002)) {
        f_closedir(&udir);
        return false;
    }

    while (f_readdir(&udir, &ufno) == FR_OK && ufno.fname[0] != 0) {
        if (ufno.fattrib & AM_DIR) continue;
        size_t len = strlen(ufno.fname);
        if (len < 5 || strcmp(&ufno.fname[len - 4], ".dat") != 0) continue;

        char cap_path[160];
        snprintf(cap_path, sizeof(cap_path), "%s/%s", user_path, ufno.fname);

        uint8_t target_id = SIL_WORD_ID;
        uint8_t word_seq[PHONEME_SEQ_LEN] = {0};
        uint8_t expected_phonemes[PHONEME_SEQ_LEN] = {0};
        uint8_t expected_phoneme_count = 0;
        char word_name[32];
        strncpy(word_name, ufno.fname, sizeof(word_name) - 1);
        word_name[sizeof(word_name) - 1] = '\0';
        char *dot = strrchr(word_name, '.');
        if (dot) *dot = '\0';
        dict_target_from_word(word_name, &target_id);
        if (dict_seq_from_word(word_name, word_seq)) {
            expected_phoneme_count = build_expected_phoneme_list(word_seq,
                                                                 expected_phonemes,
                                                                 PHONEME_SEQ_LEN);
        }
        bool expected_gender_male = (strcasecmp_local(current_user.gender, "Male") == 0);

        lcd_clear();
        lcd_print_padded_line(0, "Stage 2 ANN Train");
        char line1[21];
        snprintf(line1, sizeof(line1), "Word:%s", word_name);
        lcd_print_padded_line(1, line1);
        lcd_print_padded_line(2, last_result);
        char line3[21];
        snprintf(line3, sizeof(line3), "Count:%u", (unsigned)(trained_count + 1));
        lcd_print_padded_line(3, line3);

        uint8_t best_conf = 0;
        uint8_t best_phoneme_order = 0;
        bool gender_ok = false;
        bool user_ok = false;
        uint8_t last_max_id = 0;
        uint8_t last_user_id = 0;
        uint8_t epochs_used = 0;
        bool word_ok = run_backprop_on_file(addr,
                                            cap_path,
                                            word_name,
                                            current_user.username,
                                            target_id,
                            expected_phonemes,
                            expected_phoneme_count,
                            current_user.user_id,
                            expected_gender_male,
                                            &best_conf,
                            &best_phoneme_order,
                            &gender_ok,
                            &user_ok,
                                            &last_max_id,
                                            &last_user_id,
                                            &epochs_used);
        trained_count++;

        if (word_ok) {
            passed_count++;
            snprintf(last_result,
                     sizeof(last_result),
                     "Last:%3u%% E%u",
                     (unsigned)((best_conf * 100u) / 255u),
                     (unsigned)epochs_used);
        } else {
            failed_count++;
            snprintf(last_result,
                     sizeof(last_result),
                     "Last:%3u%% M%02X",
                     (unsigned)((best_conf * 100u) / 255u),
                     (unsigned)last_max_id);
            ok = false;
        }

        char word_summary[200];
        snprintf(word_summary,
                 sizeof(word_summary),
                 "ANNTRAIN_SUMMARY word=%s result=%s target=%u%% phon=%u%% g=%c u=%c epochs=%u max_id=0x%02X user=%u",
                 word_name,
                 word_ok ? "PASS" : "FAIL",
                 (unsigned)((best_conf * 100u) / 255u),
                 (unsigned)best_phoneme_order,
                 gender_ok ? 'Y' : 'N',
                 user_ok ? 'Y' : 'N',
                 (unsigned)epochs_used,
                 (unsigned)last_max_id,
                 (unsigned)last_user_id);
        ann_log_emit(current_user.username, word_summary);

        lcd_clear();
        lcd_print_padded_line(0, "Stage 2 ANN Train");
        char done_line1[21];
        snprintf(done_line1, sizeof(done_line1), "Word:%s", word_name);
        lcd_print_padded_line(1, done_line1);
        char done_line2[21];
        snprintf(done_line2,
                 sizeof(done_line2),
                 "T:%3u%% U:%u",
                 (unsigned)((best_conf * 100u) / 255u),
                 (unsigned)last_user_id);
        lcd_print_padded_line(2, done_line2);
        lcd_print_padded_line(3, last_result);
    }

    f_closedir(&udir);

    stage2_write_reg16(addr, STAGE2_REG_CONTROL, 0x0000);

    char overall_summary[220];
    snprintf(overall_summary,
             sizeof(overall_summary),
             "ANNTRAIN_DONE user=%s total=%u pass=%u fail=%u",
             current_user.username,
             (unsigned)trained_count,
             (unsigned)passed_count,
             (unsigned)failed_count);
    ann_log_emit(current_user.username, overall_summary);

    if (!ok || trained_count == 0) return false;

    char nn_path[64];
    if (!stage2_save_nn_to_sd(addr, nn_path, sizeof(nn_path), false)) return false;

    for (uint8_t i = 0; i < STAGE2_COUNT; i++) {
        if (i == TRAIN_BEAM_INDEX) continue;
        uint8_t other = (uint8_t)(STAGE2_BASE_ADDR + i);
        if (!stage2_load_nn_from_sd(other, nn_path)) {
            ok = false;
        }
    }

    return ok;
}

static void menu_handle_key(char key) {
    switch (menu_state) {
        case MENU_SCREEN0:
            if (key == '#') {
                menu_state = MENU_MAIN;
                menu_main_page = 0;
                menu_render_main();
            }
            break;

        case MENU_MAIN:
            if (key == '1') {
                menu_state = MENU_NEW_USER;
                add_user_start();
            } else if (menu_main_page == 0 && key == '2') {
                menu_state = MENU_SELECT_USER;
                user_menu_start();
            } else if (menu_main_page == 0 && key == '3') {
                if (current_user.set) {
                    menu_state = MENU_TRAIN_CAPTURE;
                    training_start();
                    menu_render_training_menu();
                } else {
                    lcd_set_status("Status: select user");
                    menu_state = MENU_SCREEN0;
                    menu_render_screen0();
                }
            } else if (menu_main_page == 1 && key == '4') {
                load_unrecognised_preview();
                menu_state = MENU_SELECT_UNREC;
                menu_render_unrec_select();
            } else if (menu_main_page == 1 && key == '5') {
                menu_state = MENU_SPEECH_GEN_TRAIN;
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_print("SpeechGen Train");
                if (run_speech_generator_training()) {
                    lcd_set_cursor(0, 1);
                    lcd_print("Done");
                    lcd_set_status("Status: SG train OK");
                } else {
                    lcd_set_cursor(0, 1);
                    lcd_print("Failed");
                    lcd_set_status("Status: SG train fail");
                }
                sleep_ms(1200);
                menu_state = MENU_MAIN;
                menu_main_page = 1;
                menu_render_main();
            } else if (menu_main_page == 1 && key == '6') {
                menu_state = MENU_STAGE2_ANN_CONFIRM;
                menu_render_stage2_ann_confirm();
            } else if (menu_main_page == 2 && key == '7') {
                menu_state = MENU_SAVE_ANN_CONFIRM;
                menu_render_save_ann_confirm();
            } else if (menu_main_page == 2 && key == '8') {
                if (!load_ann_menu_start()) {
                    lcd_set_status("Status: ANN list err");
                    menu_state = MENU_MAIN;
                    menu_main_page = 2;
                    menu_render_main();
                } else {
                    menu_state = MENU_LOAD_ANN_SELECT;
                    menu_render_load_ann_select();
                }
            } else if (key == 'B' && menu_main_page == 0) {
                menu_main_page = 1;
                menu_render_main();
            } else if (key == 'B' && menu_main_page == 1) {
                menu_main_page = 2;
                menu_render_main();
            } else if (key == 'A' && menu_main_page == 1) {
                menu_main_page = 0;
                menu_render_main();
            } else if (key == 'A' && menu_main_page == 2) {
                menu_main_page = 1;
                menu_render_main();
            } else if (key == '*') {
                menu_state = MENU_SCREEN0;
                menu_render_screen0();
            }
            break;

        case MENU_SELECT_USER:
            if (key == 'A' && user_menu_count > 0) {
                user_menu_index = (user_menu_index == 0) ? (uint8_t)(user_menu_count - 1) : (uint8_t)(user_menu_index - 1);
                menu_render_user_menu();
            } else if (key == 'B' && user_menu_count > 0) {
                user_menu_index = (uint8_t)((user_menu_index + 1) % user_menu_count);
                menu_render_user_menu();
            } else if (key == '#') {
                if (user_menu_count > 0) {
                    memset(&current_user, 0, sizeof(current_user));
                    make_username_from_name(user_menu_names[user_menu_index],
                                            user_menu_ids[user_menu_index],
                                            current_user.username,
                                            sizeof(current_user.username));
                    current_user.user_id = user_menu_ids[user_menu_index];
                    strncpy(current_user.full_name, user_menu_names[user_menu_index], sizeof(current_user.full_name) - 1);
                    current_user.full_name[sizeof(current_user.full_name) - 1] = '\0';
                    current_user.set = true;
                    lcd_set_status("Status: user selected");
                }
                menu_state = MENU_SCREEN0;
                menu_render_screen0();
            } else if (key == '*') {
                menu_state = MENU_MAIN;
                menu_render_main();
            }
            break;

        case MENU_NEW_USER:
            if (key == '*') {
                menu_state = MENU_MAIN;
                menu_render_main();
                break;
            }

            if (add_user_step == ADD_USER_STEP_NAME) {
                if (key == 'A') {
                    add_user_name[add_user_cursor] = add_user_next_char(add_user_name[add_user_cursor]);
                    menu_render_add_user();
                } else if (key == 'B') {
                    add_user_name[add_user_cursor] = add_user_prev_char(add_user_name[add_user_cursor]);
                    menu_render_add_user();
                } else if (key == 'D') {
                    if (add_user_cursor + 1 < ADD_USER_NAME_MAX) {
                        add_user_cursor++;
                        if (add_user_cursor >= add_user_name_len) {
                            add_user_name[add_user_cursor] = 'A';
                            add_user_name[add_user_cursor + 1] = '\0';
                            add_user_name_len = (uint8_t)(add_user_cursor + 1);
                        }
                        menu_render_add_user();
                    }
                } else if (key == 'C') {
                    if (add_user_cursor > 0) {
                        add_user_cursor--;
                        menu_render_add_user();
                    }
                } else if (key == '#') {
                    add_user_step = ADD_USER_STEP_GENDER;
                    menu_render_add_user();
                }
            } else if (add_user_step == ADD_USER_STEP_GENDER) {
                if (key == 'A' || key == 'B') {
                    add_user_gender_male = !add_user_gender_male;
                    menu_render_add_user();
                } else if (key == '#') {
                    add_user_step = ADD_USER_STEP_LANGUAGE;
                    menu_render_add_user();
                }
            } else {
                if (key == 'A' || key == 'B') {
                    uint8_t count = language_record_count();
                    if (count == 0) count = 1;

                    if (key == 'A') {
                        add_user_lang_index = (uint8_t)((add_user_lang_index + 1) % count);
                    } else {
                        add_user_lang_index = (add_user_lang_index == 0) ? (uint8_t)(count - 1) : (uint8_t)(add_user_lang_index - 1);
                    }

                    language_name_from_index(add_user_lang_index, add_user_language, sizeof(add_user_language));
                    menu_render_add_user();
                } else if (key == '#') {
                    memset(&current_user, 0, sizeof(current_user));
                    make_username_from_name(add_user_name, add_user_id, current_user.username, sizeof(current_user.username));
                    current_user.user_id = add_user_id;
                    strncpy(current_user.full_name, add_user_name, sizeof(current_user.full_name) - 1);
                    current_user.full_name[sizeof(current_user.full_name) - 1] = '\0';
                    strncpy(current_user.gender, add_user_gender_male ? "Male" : "Female", sizeof(current_user.gender) - 1);
                    current_user.gender[sizeof(current_user.gender) - 1] = '\0';
                    strncpy(current_user.language, add_user_language, sizeof(current_user.language) - 1);
                    current_user.language[sizeof(current_user.language) - 1] = '\0';
                    current_user.age = 0;
                    current_user.set = true;

                    bool list_ok = user_list_set_name(add_user_id, add_user_name);
                    bool profile_ok = user_folder_prepare(&current_user);

                    if (!list_ok || !profile_ok) {
                        output_send_line("ERROR: Add user save failed");
                        lcd_set_status("SD ERR: user save");
                    } else {
                        lcd_set_status("Status: user added");
                    }

                    menu_state = MENU_MAIN;
                    menu_render_main();
                }
            }
            break;

        case MENU_SELECT_UNREC:
            if (key >= '1' && key <= '3') {
                uint8_t idx = (uint8_t)(key - '1');
                if (idx < unrec_preview_count) {
                    char line[64];
                    snprintf(line, sizeof(line), "Selected unrec: %s", unrec_preview[idx]);
                    output_send_line(line);
                    lcd_set_status("Status: unrec selected");
                }
                menu_state = MENU_MAIN;
                menu_render_main();
            } else if (key == '*') {
                menu_state = MENU_MAIN;
                menu_render_main();
            }
            break;

        case MENU_TRAIN_CAPTURE:
            if (key == '*') {
                training_abort_to_main();
                lcd_set_status("Status: training stop");
                break;
            }

            if (!training_words_loaded || training_word_count == 0) {
                break;
            }

            if (train_state == TRAIN_IDLE) {
                if (key == 'A' || key == 'C') {
                    training_word_index = (training_word_index == 0)
                                           ? (uint16_t)(training_word_count - 1)
                                           : (uint16_t)(training_word_index - 1);
                    menu_render_training_menu();
                } else if (key == 'B' || key == 'D') {
                    training_word_index = (uint16_t)((training_word_index + 1) % training_word_count);
                    menu_render_training_menu();
                } else if (key == '#') {
                    if (!training_begin_capture()) {
                        lcd_set_status("Status: train start err");
                        menu_render_training_menu();
                    }
                }
            }
            break;

        case MENU_STAGE2_ANN_CONFIRM:
            if (key == '*') {
                menu_state = MENU_MAIN;
                menu_main_page = 1;
                menu_render_main();
            } else if (key == '#') {
                lcd_clear();
                lcd_print_padded_line(0, "Stage 2 ANN Train");
                lcd_print_padded_line(1, "Starting...");
                lcd_print_padded_line(2, "");
                lcd_print_padded_line(3, "");

                bool ann_ok = run_backprop_training();
                if (ann_ok) {
                    lcd_set_status("Status: ANN train OK");
                } else {
                    lcd_set_status("Status: ANN train FAIL");
                }

                sleep_ms(1200);
                menu_state = MENU_MAIN;
                menu_main_page = 1;
                menu_render_main();
            }
            break;

        case MENU_SAVE_ANN_CONFIRM:
            if (key == '*') {
                menu_state = MENU_MAIN;
                menu_main_page = 2;
                menu_render_main();
            } else if (key == '#') {
                uint8_t addr = (uint8_t)(STAGE2_BASE_ADDR + TRAIN_BEAM_INDEX);
                char ann_path[80];
                bool save_ok = stage2_save_nn_to_sd(addr, ann_path, sizeof(ann_path), true);

                uint8_t version = 0;
                nn_version_from_path(ann_path, &version);
                if (save_ok) {
                    lcd_set_status("Status: ANN save OK");
                    lcd_clear();
                    char line0[21];
                    snprintf(line0, sizeof(line0), "Saved ANN v%02u", (unsigned)version);
                    lcd_print_padded_line(0, line0);
                    lcd_print_padded_line(1, "RecognizerANN file");
                    lcd_print_padded_line(2, "Save complete");
                    lcd_print_padded_line(3, "Returning menu...");
                } else {
                    lcd_set_status("Status: ANN save FAIL");
                    lcd_clear();
                    lcd_print_padded_line(0, "Save ANN Failed");
                    lcd_print_padded_line(1, "Check Stage2/SD");
                    lcd_print_padded_line(2, "");
                    lcd_print_padded_line(3, "Returning menu...");
                }

                sleep_ms(1200);
                menu_state = MENU_MAIN;
                menu_main_page = 2;
                menu_render_main();
            }
            break;

        case MENU_LOAD_ANN_SELECT:
            if (key == '*') {
                menu_state = MENU_MAIN;
                menu_main_page = 2;
                menu_render_main();
            } else if (ann_version_count > 0 && (key == 'A' || key == 'B')) {
                if (key == 'A') {
                    ann_version_index = (ann_version_index == 0)
                                            ? (uint8_t)(ann_version_count - 1)
                                            : (uint8_t)(ann_version_index - 1);
                } else {
                    ann_version_index = (uint8_t)((ann_version_index + 1) % ann_version_count);
                }
                menu_render_load_ann_select();
            } else if (ann_version_count > 0 && key == '#') {
                uint8_t selected_version = ann_versions[ann_version_index];
                bool load_ok = load_ann_to_all_stage2(selected_version);

                if (load_ok) {
                    lcd_set_status("Status: ANN load OK");
                    lcd_clear();
                    char line0[21];
                    snprintf(line0, sizeof(line0), "Loaded ANN v%02u", (unsigned)selected_version);
                    lcd_print_padded_line(0, line0);
                    lcd_print_padded_line(1, "All Stage2 updated");
                    lcd_print_padded_line(2, "Upload complete");
                    lcd_print_padded_line(3, "Returning menu...");
                } else {
                    lcd_set_status("Status: ANN load FAIL");
                    lcd_clear();
                    lcd_print_padded_line(0, "Load ANN Failed");
                    lcd_print_padded_line(1, "Check file/I2C");
                    lcd_print_padded_line(2, "");
                    lcd_print_padded_line(3, "Returning menu...");
                }

                sleep_ms(1200);
                menu_state = MENU_MAIN;
                menu_main_page = 2;
                menu_render_main();
            }
            break;

        default:
            break;
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(1500);

    init_mode_pins();
    init_word_ready_pins();
    init_fault_pins();
    init_i2c_stage2();
    init_spi_sd();
    init_uart_ttl();
    lcd_init();

    for (int i = 0; i < WORD_HISTORY_COUNT; i++) {
        word_history[i][0] = '\0';
    }
    word_history_count = 0;
    lcd_set_status("Status: Booting");
    menu_render_screen0();

    output_send_line("Speech Recognition Translator starting...");

    // Initialize SD card and dictionary
    if (dict_init()) {
        output_send_line("Dictionary loaded successfully");
        lcd_set_status("Status: Ready");
        generate_sample_words();
    } else {
        output_send_line("ERROR: Failed to load dictionary");
        lcd_set_status("SD ERR: dict init");
    }
    menu_render_screen0();

    while (1) {
        char line[160];
        if (read_line(line, sizeof(line))) {
            handle_command(line);
        }

        static absolute_time_t last_key = {0};
        if (absolute_time_diff_us(last_key, get_absolute_time()) > 100000) {
            last_key = get_absolute_time();
            char key = keypad_get_key();
            if (key) {
                menu_handle_key(key);
            }
        }

        if (train_state != TRAIN_IDLE) {
            training_tick();
            tight_loop_contents();
            continue;
        }

        for (uint8_t i = 0; i < STAGE2_COUNT; i++) {
            uint8_t addr = (uint8_t)(STAGE2_BASE_ADDR + i);
            bool word_ready = !gpio_get(word_ready_pins[i]);
            if (!word_ready) {
                continue;
            }

            uint16_t fifo_len = 0;
            if (!stage2_read_fifo_len(addr, &fifo_len)) {
                gpio_put(stage2_fault_pins[i], 1);
                continue;
            }
            gpio_put(stage2_fault_pins[i], 0);

            for (uint16_t n = 0; n < fifo_len; n++) {
                stage2_entry_t entry;
                if (!stage2_read_fifo_entry(addr, &entry)) {
                    gpio_put(stage2_fault_pins[i], 1);
                    break;
                }
                handle_stage2_entry(i, &entry);
            }
        }

        tight_loop_contents();
    }

    return 0;
}
