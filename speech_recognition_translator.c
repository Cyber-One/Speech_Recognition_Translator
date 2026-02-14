#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
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

// Stage 2 registers
#define STAGE2_REG_CONTROL    0x00
#define STAGE2_REG_FIFO_LEN   0x01
#define STAGE2_REG_FIFO_READ  0x05
#define STAGE2_REG_TARGET_NEURON 0x04
#define STAGE2_REG_PAGE_MODE  0x0C
#define STAGE2_REG_PAGE_ADDR  0x0D
#define STAGE2_REG_PAGE_LEN   0x0E
#define STAGE2_REG_PAGE_DATA  0x0F

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
    MENU_HOME = 0,
    MENU_SELECT_USER,
    MENU_NEW_USER,
    MENU_TRAIN_CAPTURE,
    MENU_BACKPROP
} menu_state_t;

static menu_state_t menu_state = MENU_HOME;
static char input_buffer_line[32];
static uint8_t input_len = 0;
static uint8_t new_user_step = 0;
static char pending_full_name[64];
static char pending_gender[8];
static uint8_t pending_age = 0;

// LCD recent words (channel 2)
#define RECENT_WORDS 4
static char recent_lines[RECENT_WORDS][21];
static uint8_t recent_count = 0;

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

// Dictionary format: 2-byte language ID + 15-byte phoneme seq + 25-byte word
#define DICT_RECORD_SIZE 42
#define DICT_LANG_ID_SIZE 2
#define DICT_PHONEME_SIZE 15
#define DICT_WORD_SIZE 25

// Language file format: 2-byte ID + 30-byte name
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
#define CAPTURE_FRAMES 80
#define MAX_WORD_LEN 24
#define MAX_PHONEMES_PER_WORD 8

// Forward declaration of training state
typedef enum {
    TRAIN_IDLE = 0,
    TRAIN_SHOW_WORD,
    TRAIN_WAIT_TRIGGER,
    TRAIN_CAPTURE,
    TRAIN_SAVE,
    TRAIN_NEXT
} train_state_t;

static train_state_t train_state = TRAIN_IDLE;

typedef struct {
    char username[32];
    char full_name[64];
    uint8_t age;
    char gender[8];
    bool set;
} user_profile_t;

static user_profile_t current_user = {0};

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
    return (train_state == TRAIN_IDLE) && (menu_state == MENU_HOME);
}

static void lcd_update_recent(void) {
    if (!lcd_available_for_status()) return;
    lcd_clear();
    for (uint8_t i = 0; i < RECENT_WORDS; i++) {
        lcd_set_cursor(0, i);
        lcd_print(recent_lines[i]);
    }
}

static void recent_words_push(const char *word, const char *gender) {
    char line[21];
    snprintf(line, sizeof(line), "%s %s", word, gender);

    for (int i = RECENT_WORDS - 1; i > 0; i--) {
        strncpy(recent_lines[i], recent_lines[i - 1], sizeof(recent_lines[i]));
        recent_lines[i][20] = '\0';
    }
    strncpy(recent_lines[0], line, sizeof(recent_lines[0]) - 1);
    recent_lines[0][20] = '\0';

    if (recent_count < RECENT_WORDS) recent_count++;
    lcd_update_recent();
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

static void menu_render_home(void) {
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print("A:Select User");
    lcd_set_cursor(0, 1);
    lcd_print("B:New User");
    lcd_set_cursor(0, 2);
    lcd_print("C:Capture");
    lcd_set_cursor(0, 3);
    lcd_print("D:Backprop");
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
    
    // Language record format: 2-byte ID + 30-byte name (null-padded)
    uint8_t record[LANG_RECORD_SIZE];
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
        memset(record, 0, LANG_RECORD_SIZE);
        
        // Write language ID (little-endian)
        record[0] = languages[i].id & 0xFF;
        record[1] = (languages[i].id >> 8) & 0xFF;
        
        // Write language name (null-terminated, padded)
        strncpy((char *)&record[LANG_ID_SIZE], languages[i].name, LANG_NAME_SIZE - 1);
        
        res = f_write(&lang_file, record, LANG_RECORD_SIZE, &bw);
        if (res != FR_OK || bw != LANG_RECORD_SIZE) {
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
    
    // Create record: 2-byte language ID + 15-byte phoneme seq + 25-byte word
    uint8_t record[DICT_RECORD_SIZE];
    memset(record, 0, DICT_RECORD_SIZE);
    
    // Language ID = 0 (unknown)
    record[0] = LANG_UNKNOWN & 0xFF;
    record[1] = (LANG_UNKNOWN >> 8) & 0xFF;
    
    // Copy phoneme sequence
    memcpy(&record[DICT_LANG_ID_SIZE], seq, PHONEME_SEQ_LEN);
    
    // Generate word: "UnRecognisedXX"
    char word[DICT_WORD_SIZE];
    snprintf(word, DICT_WORD_SIZE, "UnRecognised%02d", unrecognised_counter);
    memcpy(&record[DICT_LANG_ID_SIZE + DICT_PHONEME_SIZE], word, strlen(word));
    
    // Write record
    res = f_write(&newwords, record, DICT_RECORD_SIZE, &bw);
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

static bool dict_lookup_word(const uint8_t *seq, char *word_out, size_t word_out_len) {
    if (!dict_ready || word_out_len < 2) return false;

    // Record format: 2 bytes language ID + 15 bytes phoneme IDs + 25 bytes ASCII word (null-padded)
    // Total: 42 bytes per record
    // Dictionary.dat is sorted by phoneme sequence (binary search possible)
    // NewWords.dat is sequential (linear search required)

    uint8_t record[DICT_RECORD_SIZE];
    UINT br;
    FRESULT res;

    // First, search Dictionary.dat (sorted, can use binary search or linear with early exit)
    res = f_lseek(&dict_file, 0);
    if (res != FR_OK) return false;

    while (1) {
        res = f_read(&dict_file, record, DICT_RECORD_SIZE, &br);
        if (res != FR_OK || br < DICT_RECORD_SIZE) break;

        // Compare phoneme sequence (15 bytes starting at offset 2, after language ID)
        bool match = true;
        for (int i = 0; i < PHONEME_SEQ_LEN; i++) {
            if (record[DICT_LANG_ID_SIZE + i] != seq[i]) {
                match = false;
                break;
            }
        }

        if (match) {
            // Found it! Extract word (25 bytes starting at offset 17)
            strncpy(word_out, (char *)&record[DICT_LANG_ID_SIZE + DICT_PHONEME_SIZE], word_out_len - 1);
            word_out[word_out_len - 1] = '\0';
            return true;
        }

        // If sequence is lexicographically larger, dictionary is sorted so no match
        bool is_less = false;
        for (int i = 0; i < PHONEME_SEQ_LEN; i++) {
            if (seq[i] < record[DICT_LANG_ID_SIZE + i]) {
                is_less = true;
                break;
            } else if (seq[i] > record[DICT_LANG_ID_SIZE + i]) {
                break;
            }
        }
        if (is_less) break;  // Dictionary is past our target, not in Dictionary.dat
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

        // Compare phoneme sequence
        bool match = true;
        for (int i = 0; i < PHONEME_SEQ_LEN; i++) {
            if (record[DICT_LANG_ID_SIZE + i] != seq[i]) {
                match = false;
                break;
            }
        }

        if (match) {
            // Found it in NewWords.dat
            strncpy(word_out, (char *)&record[DICT_LANG_ID_SIZE + DICT_PHONEME_SIZE], word_out_len - 1);
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
    const char *prefix = "NNdata_";
    size_t len = strlen(name);
    if (len != 13) return false; // NNdata_XX.dat
    if (strncmp(name, prefix, 7) != 0) return false;
    if (name[9] != '.' || name[10] != 'd' || name[11] != 'a' || name[12] != 't') return false;
    if (name[7] < '0' || name[7] > '9' || name[8] < '0' || name[8] > '9') return false;
    *index_out = (uint8_t)((name[7] - '0') * 10 + (name[8] - '0'));
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
    snprintf(path_out, path_len, "0:/microsd/NNdata_%02u.dat", next);
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
    int n = snprintf(line, sizeof(line), "Name: %s\r\nAge: %u\r\nGender: %s\r\n",
                     user->full_name, user->age, user->gender);
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

    bool progress = true;
    while (progress) {
        progress = false;
        uint8_t best_record[DICT_RECORD_SIZE];
        int best_gain = 0;
        char best_word[26] = {0};

        f_lseek(&dict, 0);
        UINT br = 0;
        uint8_t record[DICT_RECORD_SIZE];
        while (1) {
            res = f_read(&dict, record, DICT_RECORD_SIZE, &br);
            if (res != FR_OK || br < DICT_RECORD_SIZE) break;

            // Phoneme sequence starts at offset 2 (after 2-byte language ID)
            int pcount = phoneme_count(&record[DICT_LANG_ID_SIZE]);
            if (pcount == 0 || pcount > MAX_PHONEMES_PER_WORD) continue;

            // Word starts at offset 17 (2-byte lang ID + 15-byte phoneme seq)
            char word[26];
            memcpy(word, &record[DICT_LANG_ID_SIZE + DICT_PHONEME_SIZE], DICT_WORD_SIZE);
            word[DICT_WORD_SIZE] = '\0';
            size_t wlen = strnlen(word, DICT_WORD_SIZE);
            if (!word_is_short(word, wlen)) continue;

            int gain = 0;
            for (int i = 0; i < PHONEME_SEQ_LEN; i++) {
                uint8_t id = record[DICT_LANG_ID_SIZE + i];
                if (id >= phoneme_min && id <= phoneme_max) {
                    int idx = id - phoneme_min;
                    if (counts[idx] < 3) gain++;
                }
            }

            if (gain > best_gain) {
                best_gain = gain;
                memcpy(best_record, record, DICT_RECORD_SIZE);
                strncpy(best_word, word, sizeof(best_word) - 1);
            }
        }

        if (best_gain > 0) {
            UINT bw = 0;
            f_write(&out, best_word, (UINT)strlen(best_word), &bw);
            f_write(&out, "\r\n", 2, &bw);

            for (int i = 0; i < PHONEME_SEQ_LEN; i++) {
                uint8_t id = best_record[DICT_LANG_ID_SIZE + i];
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

    // Open Dictionary.dat in append mode
    FIL dict;
    res = f_open(&dict, "0:/microsd/Dictionary.dat", FA_WRITE | FA_OPEN_APPEND);
    if (res != FR_OK) {
        printf("ERROR: Failed to open Dictionary.dat for append (code %d)\\n", res);
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
    UINT br, bw;
    uint32_t merged_count = 0;

    // Copy all records from NewWords.dat to Dictionary.dat
    while (1) {
        res = f_read(&newwords, record, DICT_RECORD_SIZE, &br);
        if (res != FR_OK || br < DICT_RECORD_SIZE) break;

        res = f_write(&dict, record, DICT_RECORD_SIZE, &bw);
        if (res != FR_OK || bw != DICT_RECORD_SIZE) {
            printf("ERROR: Failed to write merged record\\n");
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

static bool dict_target_from_word(const char *word, uint8_t *target_out) {
    if (!dict_ready) return false;

    FIL dict;
    if (f_open(&dict, "0:/microsd/Dictionary.dat", FA_READ | FA_OPEN_EXISTING) != FR_OK) return false;

    UINT br = 0;
    uint8_t record[DICT_RECORD_SIZE];
    bool found = false;

    while (1) {
        if (f_read(&dict, record, DICT_RECORD_SIZE, &br) != FR_OK || br < DICT_RECORD_SIZE) break;

        // Extract word (25 bytes starting at offset 17: 2-byte lang ID + 15-byte phoneme seq)
        char w[26];
        memcpy(w, &record[DICT_LANG_ID_SIZE + DICT_PHONEME_SIZE], DICT_WORD_SIZE);
        w[DICT_WORD_SIZE] = '\0';
        if (strcasecmp_local(w, word) == 0) {
            // Find first valid phoneme ID in sequence (offset 2 for lang ID)
            for (int i = 0; i < PHONEME_SEQ_LEN; i++) {
                uint8_t id = record[DICT_LANG_ID_SIZE + i];
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
static FIL sample_file;
static bool sample_file_open = false;
static char current_word[32];
static uint8_t capture_buffer[CAPTURE_FRAMES][INPUT_NEURONS];
static uint16_t capture_index = 0;

static uint8_t peak_window[PEAK_WINDOW_FRAMES];
static uint16_t peak_sum = 0;
static uint16_t peak_pos = 0;

static void peak_window_reset(void) {
    memset(peak_window, 0, sizeof(peak_window));
    peak_sum = 0;
    peak_pos = 0;
}

static uint8_t compute_peak(const uint8_t *frame) {
    uint8_t peak = 0;
    for (int i = 0; i < INPUT_NEURONS; i++) {
        if (frame[i] > peak) peak = frame[i];
    }
    return peak;
}

static bool sample_words_open(void) {
    if (sample_file_open) return true;
    FRESULT res = f_open(&sample_file, "0:/microsd/SampleWords.txt", FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK) return false;
    sample_file_open = true;
    return true;
}

static bool sample_words_next(char *word_out, size_t len) {
    if (!sample_file_open) return false;
    char line[64];
    while (f_gets(line, sizeof(line), &sample_file)) {
        size_t l = strcspn(line, "\r\n");
        line[l] = '\0';
        if (l > 0) {
            strncpy(word_out, line, len - 1);
            word_out[len - 1] = '\0';
            return true;
        }
    }
    return false;
}

static bool save_capture_to_sd(const user_profile_t *user, const char *word) {
    if (!user || !user->set) return false;
    if (!user_folder_prepare(user)) return false;

    char path[160];
    snprintf(path, sizeof(path), "0:/microsd/%s/%s.cap", user->username, word);

    FILINFO fno;
    if (f_stat(path, &fno) == FR_OK) {
        for (int i = 1; i < 100; i++) {
            snprintf(path, sizeof(path), "0:/microsd/%s/%s_%02d.cap", user->username, word, i);
            if (f_stat(path, &fno) != FR_OK) break;
        }
    }

    FIL file;
    FRESULT res = f_open(&file, path, FA_WRITE | FA_CREATE_NEW);
    if (res != FR_OK) return false;

    UINT bw = 0;
    uint8_t header[8] = {'C','A','P','0', (uint8_t)INPUT_NEURONS, 0, (uint8_t)CAPTURE_FRAMES, 0};
    res = f_write(&file, header, sizeof(header), &bw);
    if (res != FR_OK || bw != sizeof(header)) {
        f_close(&file);
        return false;
    }

    for (uint16_t i = 0; i < CAPTURE_FRAMES; i++) {
        res = f_write(&file, capture_buffer[i], INPUT_NEURONS, &bw);
        if (res != FR_OK || bw != INPUT_NEURONS) {
            f_close(&file);
            return false;
        }
    }

    f_close(&file);
    return true;
}

static void training_start(void) {
    if (!sd_ready || !current_user.set) return;
    if (!sample_words_open()) return;

    train_state = TRAIN_SHOW_WORD;
    peak_window_reset();
    capture_index = 0;
}

static void training_stop(void) {
    if (sample_file_open) {
        f_close(&sample_file);
        sample_file_open = false;
    }
    train_state = TRAIN_IDLE;
    menu_state = MENU_HOME;
    menu_render_home();
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
        case TRAIN_SHOW_WORD: {
            if (!sample_words_next(current_word, sizeof(current_word))) {
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_print("Training Done");
                training_stop();
                break;
            }

            stage2_clear_input(addr);
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_print("Speak word:");
            lcd_set_cursor(0, 1);
            lcd_print(current_word);

            peak_window_reset();
            capture_index = 0;
            train_state = TRAIN_WAIT_TRIGGER;
            break;
        }
        case TRAIN_WAIT_TRIGGER:
            if (peak > avg_peak) {
                capture_index = 0;
                train_state = TRAIN_CAPTURE;
            }
            break;
        case TRAIN_CAPTURE:
            if (capture_index < CAPTURE_FRAMES) {
                memcpy(capture_buffer[capture_index], frame, INPUT_NEURONS);
                capture_index++;
            }
            if (capture_index >= CAPTURE_FRAMES) {
                stage2_write_reg16(addr, STAGE2_REG_CONTROL, STAGE2_CTRL_FREEZE_PAUSE);
                train_state = TRAIN_SAVE;
            }
            break;
        case TRAIN_SAVE:
            if (save_capture_to_sd(&current_user, current_word)) {
                stage2_write_reg16(addr, STAGE2_REG_CONTROL, 0x0000);
                train_state = TRAIN_NEXT;
            } else {
                stage2_write_reg16(addr, STAGE2_REG_CONTROL, 0x0000);
                training_stop();
            }
            break;
        case TRAIN_NEXT:
            train_state = TRAIN_SHOW_WORD;
            break;
        default:
            break;
    }
}

static bool stage2_save_nn_to_sd(uint8_t addr, char *path_out, size_t path_len) {
    if (!sd_ready) return false;
    if (!nn_next_filename(path_out, path_len)) return false;

    if (!stage2_write_reg16(addr, STAGE2_REG_CONTROL, STAGE2_CTRL_FREEZE_PAUSE)) return false;
    sleep_ms(5);

    uint8_t buffer[NN_TOTAL_SIZE];
    uint8_t *ptr = buffer;

    if (!stage2_page_read(addr, STAGE2_PAGE_W1, 0, W1_SIZE, ptr)) goto cleanup;
    ptr += W1_SIZE;
    if (!stage2_page_read(addr, STAGE2_PAGE_B1, 0, B1_SIZE, ptr)) goto cleanup;
    ptr += B1_SIZE;
    if (!stage2_page_read(addr, STAGE2_PAGE_W2, 0, W2_SIZE, ptr)) goto cleanup;
    ptr += W2_SIZE;
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

    char word[26];
    if (dict_lookup_word(seq->seq, word, sizeof(word))) {
        const char *gender = (entry->female_val >= entry->male_val) ? "female" : "male";
        uint8_t conf = (entry->female_val >= entry->male_val) ? entry->female_val : entry->male_val;
        char line[160];
        snprintf(line, sizeof(line), "beam=%u user_id=%u user=%s word=%s gender=%s conf=%u",
                 beam_idx, entry->user_id, user_name, word, gender, conf);
        output_send_line(line);
        if (beam_idx == TRAIN_BEAM_INDEX) {
            recent_words_push(word, gender);
        }
        seq->count = 0; // reset after a match
    } else {
        // Word not found - add to dictionary with language ID 0 (unknown)
        if (dict_add_unknown_word(seq->seq)) {
            // Successfully added - output the new unrecognised word
            char unrec_word[26];
            snprintf(unrec_word, sizeof(unrec_word), "UnRecognised%02d", unrecognised_counter - 1);
            const char *gender = (entry->female_val >= entry->male_val) ? "female" : "male";
            uint8_t conf = (entry->female_val >= entry->male_val) ? entry->female_val : entry->male_val;
            char line[160];
            snprintf(line, sizeof(line), "beam=%u user_id=%u user=%s word=%s gender=%s conf=%u [NEW]",
                     beam_idx, entry->user_id, user_name, unrec_word, gender, conf);
            output_send_line(line);
            if (beam_idx == TRAIN_BEAM_INDEX) {
                recent_words_push(unrec_word, gender);
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

static bool run_backprop_on_file(uint8_t addr, const char *path, uint8_t target_id) {
    FIL file;
    if (f_open(&file, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) return false;

    uint8_t header[8];
    UINT br = 0;
    if (f_read(&file, header, sizeof(header), &br) != FR_OK || br != sizeof(header)) {
        f_close(&file);
        return false;
    }
    if (memcmp(header, "CAP0", 4) != 0 || header[4] != INPUT_NEURONS) {
        f_close(&file);
        return false;
    }

    uint8_t frames = header[6];
    uint8_t frame[INPUT_NEURONS];
    for (uint8_t i = 0; i < frames; i++) {
        if (f_read(&file, frame, INPUT_NEURONS, &br) != FR_OK || br != INPUT_NEURONS) {
            f_close(&file);
            return false;
        }
        if (!stage2_page_write(addr, STAGE2_PAGE_INPUT, 0, INPUT_NEURONS, frame)) {
            f_close(&file);
            return false;
        }
        stage2_set_target(addr, target_id);
        stage2_trigger_backprop(addr);
        sleep_ms(5);
    }

    f_close(&file);
    return true;
}

static bool run_backprop_training(void) {
    if (!sd_ready) return false;

    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, "0:/microsd") != FR_OK) return false;

    uint8_t addr = (uint8_t)(STAGE2_BASE_ADDR + TRAIN_BEAM_INDEX);
    bool ok = true;

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        if (!(fno.fattrib & AM_DIR)) continue;
        if (strcmp(fno.fname, ".") == 0 || strcmp(fno.fname, "..") == 0) continue;

        char user_path[128];
        snprintf(user_path, sizeof(user_path), "0:/microsd/%s", fno.fname);

        DIR udir;
        FILINFO ufno;
        if (f_opendir(&udir, user_path) != FR_OK) continue;

        while (f_readdir(&udir, &ufno) == FR_OK && ufno.fname[0] != 0) {
            if (ufno.fattrib & AM_DIR) continue;
            size_t len = strlen(ufno.fname);
            if (len < 5 || strcmp(&ufno.fname[len - 4], ".cap") != 0) continue;

            char cap_path[160];
            snprintf(cap_path, sizeof(cap_path), "%s/%s", user_path, ufno.fname);

            uint8_t target_id = SIL_WORD_ID;
            char word_name[32];
            strncpy(word_name, ufno.fname, sizeof(word_name) - 1);
            word_name[sizeof(word_name) - 1] = '\0';
            char *dot = strrchr(word_name, '.');
            if (dot) *dot = '\0';
            dict_target_from_word(word_name, &target_id);

            if (!run_backprop_on_file(addr, cap_path, target_id)) {
                ok = false;
            }
        }
        f_closedir(&udir);
    }

    f_closedir(&dir);
    if (!ok) return false;

    char nn_path[64];
    if (!stage2_save_nn_to_sd(addr, nn_path, sizeof(nn_path))) return false;

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
        case MENU_HOME:
            if (key == 'A') {
                menu_state = MENU_SELECT_USER;
                input_reset();
                menu_render_input("Select user:");
            } else if (key == 'B') {
                menu_state = MENU_NEW_USER;
                new_user_step = 0;
                input_reset();
                menu_render_input("Username:");
            } else if (key == 'C') {
                menu_state = MENU_TRAIN_CAPTURE;
                training_start();
            } else if (key == 'D') {
                menu_state = MENU_BACKPROP;
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_print("Backprop... ");
                if (run_backprop_training()) {
                    lcd_set_cursor(0, 1);
                    lcd_print("Done");
                } else {
                    lcd_set_cursor(0, 1);
                    lcd_print("Failed");
                }
                menu_state = MENU_HOME;
                menu_render_home();
            }
            break;
        case MENU_SELECT_USER:
        case MENU_NEW_USER:
            if (key == '#') {
                if (menu_state == MENU_SELECT_USER) {
                    strncpy(current_user.username, input_buffer_line, sizeof(current_user.username) - 1);
                    current_user.username[sizeof(current_user.username) - 1] = '\0';
                    current_user.set = true;
                    menu_state = MENU_HOME;
                    menu_render_home();
                } else {
                    if (new_user_step == 0) {
                        strncpy(current_user.username, input_buffer_line, sizeof(current_user.username) - 1);
                        current_user.username[sizeof(current_user.username) - 1] = '\0';
                        new_user_step = 1;
                        input_reset();
                        menu_render_input("Full name:");
                    } else if (new_user_step == 1) {
                        strncpy(pending_full_name, input_buffer_line, sizeof(pending_full_name) - 1);
                        pending_full_name[sizeof(pending_full_name) - 1] = '\0';
                        new_user_step = 2;
                        input_reset();
                        menu_render_input("Age:");
                    } else if (new_user_step == 2) {
                        pending_age = (uint8_t)atoi(input_buffer_line);
                        new_user_step = 3;
                        input_reset();
                        menu_render_input("Gender:");
                    } else if (new_user_step == 3) {
                        strncpy(pending_gender, input_buffer_line, sizeof(pending_gender) - 1);
                        pending_gender[sizeof(pending_gender) - 1] = '\0';

                        strncpy(current_user.full_name, pending_full_name, sizeof(current_user.full_name) - 1);
                        current_user.full_name[sizeof(current_user.full_name) - 1] = '\0';
                        strncpy(current_user.gender, pending_gender, sizeof(current_user.gender) - 1);
                        current_user.gender[sizeof(current_user.gender) - 1] = '\0';
                        current_user.age = pending_age;
                        current_user.set = true;

                        if (!user_folder_prepare(&current_user)) {
                            output_send_line("ERROR: User data save failed");
                        }

                        menu_state = MENU_HOME;
                        menu_render_home();
                    }
                }
            } else if (key == '*') {
                input_reset();
                if (menu_state == MENU_SELECT_USER) {
                    menu_render_input("Select user:");
                } else {
                    if (new_user_step == 0) menu_render_input("Username:");
                    else if (new_user_step == 1) menu_render_input("Full name:");
                    else if (new_user_step == 2) menu_render_input("Age:");
                    else menu_render_input("Gender:");
                }
            } else {
                input_append(key);
                if (menu_state == MENU_SELECT_USER) {
                    menu_render_input("Select user:");
                } else {
                    if (new_user_step == 0) menu_render_input("Username:");
                    else if (new_user_step == 1) menu_render_input("Full name:");
                    else if (new_user_step == 2) menu_render_input("Age:");
                    else menu_render_input("Gender:");
                }
            }
            break;
        case MENU_TRAIN_CAPTURE:
            if (key == '*') {
                training_stop();
                menu_state = MENU_HOME;
                menu_render_home();
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
    menu_render_home();

    for (int i = 0; i < RECENT_WORDS; i++) {
        recent_lines[i][0] = '\0';
    }

    output_send_line("Speech Recognition Translator starting...");

    // Initialize SD card and dictionary
    if (dict_init()) {
        output_send_line("Dictionary loaded successfully");
        generate_sample_words();
    } else {
        output_send_line("ERROR: Failed to load dictionary");
    }

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
