# Speech Recognition Translator

## Overview

This stage aggregates phoneme outputs from **five** Speech_Process_8bit_relu units, resolves phoneme sequences into text, and emits translated words with **gender** and **direction** tags. It also manages **microSD** storage for neural weights/bias and the translation dictionary.

- **I2C0 master** reads FIFO data from stage‑2 devices at **0x60–0x64**
- **GPIO inputs** (one per stage‑2) indicate valid FIFO data
- **SPI** microSD stores dictionary + stage‑2 weights/bias
- **Output mode** selectable via 2 GPIO pins (USB / TTL / I2C)
- **Diagnostic GPIOs** indicate stage‑2 device failure

## Table of Contents

- [Pin Configuration](#pin-configuration)
- [LCD Menu System (20x4)](#lcd-menu-system-20x4)
  - [Training Menu (Main Menu option 3)](#training-menu-main-menu-option-3)
- [Stage‑2 FIFO Read Protocol](#stage2-fifo-read-protocol)
- [Phoneme Buffering](#phoneme-buffering)
- [Dictionary Storage (microSD)](#dictionary-storage-microsd)
- [Translation Output](#translation-output)
- [microSD Capacity](#microsd-capacity)
- [Command Set (Stage‑2 Control)](#command-set-stage2-control)
- [Build](#build)
- [Status](#status)
- [Multilingual Support & Unknown Word Tracking](#multilingual-support--unknown-word-tracking)
  - [Two-File Architecture](#two-file-architecture)
  - [Unknown Word Lifecycle](#unknown-word-lifecycle)
  - [File Specifications](#file-specifications)
  - [Language IDs](#language-ids)
  - [API Reference](#api-reference)
  - [Usage Examples](#usage-examples)
  - [Constants](#constants)
  - [Performance Notes](#performance-notes)
  - [File Size Summary](#file-size-summary)
  - [Testing Checklist](#testing-checklist)
  - [Troubleshooting](#troubleshooting)

## Pin Configuration

### I2C0 (Stage‑2 Read)

| GPIO | Function | Description |
|------|----------|-------------|
| 20   | SDA      | I2C0 Data   |
| 21   | SCL      | I2C0 Clock  |

### Word‑Ready Inputs (one per stage‑2)

| GPIO | Function | Description               |
|------|----------|---------------------------|
| 6    | WR0      | Stage‑2 @ 0x60 word‑ready |
| 7    | WR1      | Stage‑2 @ 0x61 word‑ready |
| 8    | WR2      | Stage‑2 @ 0x62 word‑ready |
| 9    | WR3      | Stage‑2 @ 0x63 word‑ready |
| 10   | WR4      | Stage‑2 @ 0x64 word‑ready |

### Diagnostic Outputs (stage‑2 not responding)

| GPIO | Function | Description          |
|------|----------|----------------------|
| 11   | FAULT0   | Stage‑2 @ 0x60 fault |
| 12   | FAULT1   | Stage‑2 @ 0x61 fault |
| 13   | FAULT2   | Stage‑2 @ 0x62 fault |
| 14   | FAULT3   | Stage‑2 @ 0x63 fault |
| 15   | FAULT4   | Stage‑2 @ 0x64 fault |

### Output Mode Select (2 GPIOs)

| GPIO | Function | Description                   |
|------|----------|-------------------------------|
| 2    | MODE0    | Output select bit 0 (pull‑up) |
| 3    | MODE1    | Output select bit 1 (pull‑up) |

Mode table:

- `00` = USB serial
- `01` = TTL UART
- `10` = I2C output (future)

### TTL UART (Serial Output)

| GPIO | Function | Description |
|------|----------|-------------|
| 0    | TX       | UART0 TX    |
| 1    | RX       | UART0 RX    |

### microSD (SPI0)

| GPIO | Function | Description    |
|------|----------|----------------|
| 18   | SCK      | SPI0 Clock     |
| 19   | MOSI     | SPI0 MOSI      |
| 16   | MISO     | SPI0 MISO      |
| 17   | CS       | SD Chip Select |

### LCD + Keypad (shared I2C expanders)

Both the LCD and keypad use PCF8574 I/O expanders on the same I2C0 bus.

| Device                    | I2C Address | Purpose                |
|---------------------------|-------------|------------------------|
| LCD backpack (PCF8574)    | `0x27`      | 20x4 text display      |
| Keypad expander (PCF8574) | `0x26`      | 4x4 matrix keypad scan |

Beginner wiring notes:

- Keep all grounds common: Pico GND, stage-2 boards, LCD module, keypad module, SD module.
- Use 3.3 V compatible I2C devices.
- Ensure SDA/SCL have pull-ups (many modules include them already).
- Avoid address conflicts on I2C0 (`0x26`, `0x27`, `0x60`-`0x64`).

## LCD Menu System (20x4)

### Default Screen (Screen 0)

- **Line 0:** System status + microSD error status
- **Lines 1-3:** Wrapped history containing the last **10** recognized words

Press **`#`** from Screen 0 to open the menu.

### Key Mapping (Menu/Input)

- **A** = Up
- **B** = Down
- **C** = Left
- **D** = Right

### Main Menu (Paged)

- **Line 0:** `Main Menu Pg 0`, `Main Menu Pg 1`, or `Main Menu Pg 2`
- **Page 0:**
  1. **Add New User**
  2. **User Menu**
  3. **Start Training**
- **Page 1:**
  4. **Select Word from Unrecognized List**
  5. **Initiate Speech Generator Training**
  6. **Stage 2 ANN Training**
- **Page 2:**
  7. **Save ANN**
  8. **Load Speech ANN**

Navigation behavior:

- From **Page 0**, **A/C/D** have no effect.
- From **Page 0**, **B** switches to **Page 1**.
- From **Page 1**, **A** switches back to **Page 0** and **B** switches to **Page 2**.
- From **Page 2**, **A** switches back to **Page 1**.
- Press **`*`** to exit menu screens back to Screen 0.

### User Menu (from Main Menu option 2)

- **Line 0:** `User Menu`
- **Line 1:** `User ID: <id> <name>` for the current selection
- **A/B** cycles up/down through configured users
- Only assigned users are shown (unassigned/default entries are excluded)
- Press **`#`** to select the displayed user and return to Screen 0
- Press **`*`** to return to Main Menu

### Training Menu (Main Menu option 3)

This menu is available only when a user is selected.

- Press **`3`** from **Main Menu Page 0** to enter training.
- If no user is selected, training does not open and status is set to `select user`.

Display behavior:

- **Line 0:** `Training Memnu X/Y` (firmware text), where `X` is current word index and `Y` is total words
- **Line 1:** Current training word (centered)
- **Line 2:** `Recording: Present` or `Recording: Missing`
- **Line 3 (idle):** `A/B:Scroll #:Train`
- **Line 3 (armed/capturing):** `Speak When Ready` (centered)

Key behavior:

- **A** / **C** = previous word
- **B** / **D** = next word
- **#** = arm capture for currently displayed word
- **\*** = abort training and return to Main Menu Page 1

### Stage 2 ANN Training (Main Menu Page 1, option 6)

- Entry point: **Main Menu Page 1 -> 6**
- Scope: training is applied only to **Stage-2 channel/unit 2**
- Confirmation required before start:
  - **Line 0:** `Stage 2 ANN Train`
  - **Line 1:** `Are you sure?`
  - **`#`** = Yes, start ANN training
  - **`*`** = No, return to main menu

During training, the LCD shows:

- Current word being trained
- Last training result with certainty/epoch info
- Running count and latest inferred user ID

ANN training logs are also appended to microSD at:

- `microsd/logs/<username>_ann_train.log`

Training loop behavior:

- Incoming stage-1 stream is frozen before ANN training begins.
- Each training word (`.dat`) is replayed into stage-2 and backprop is triggered.
- Stage-2 telemetry is read after passes to evaluate sequence/gender/user correctness.
- The same word is retrained across epochs until all pass criteria are met (or max epoch limit is hit).

Per-word pass criteria:

1. **Target certainty** >= **80%**
2. **Phoneme order match** >= **80%** against the dictionary sequence for that word
3. **Gender check** passes (`female` or `male` output >= 80% for selected user gender)
4. **User check** passes (returned user ID matches selected user and user-neuron confidence >= 80%)

At completion, the system returns to Main Menu.

### Speech Generator Training (Main Menu Page 1, option 5)

- Entry point: **Main Menu Page 1 -> 5**
- Scope: trains **Stage 4 (`Speech_Generation`, I2C `0x65`)** against **Stage 2 channel 2 (`0x62`)**

Training loop behavior:

1. Send a **single phoneme ID** to Stage 4 and trigger image generation.
2. Capture generated Stage-4 image buffer (`40 bins x 100 lines`).
3. Replay each 40-byte line into Stage-2 channel 2 input.
4. Read Stage-2 target confidence for that phoneme.
5. If confidence < **80%**, trigger one Stage-4 backprop step and retry.

Per-phoneme pass condition:

- Stage-2 target confidence for the requested phoneme reaches **>= 80%**.

During this process, the LCD shows phoneme ID, epoch progress, and running status.

### Save ANN (Main Menu Page 2, option 7)

- Entry point: **Main Menu Page 2 -> 7**
- Scope: save is performed from **Stage-2 channel/unit 2 only**
- Confirmation required before save:
  - **Line 0:** `Save ANN`
  - **Line 1:** `Are you sure?`
  - **`#`** = Yes, start save
  - **`*`** = No, return to main menu

During save, LCD displays:

- New ANN version ID (`vXX`)
- Current save phase (W1/B1/W2/B2)
- Save progress percentage

Saved ANN file behavior:

- Path format: `microsd/RecognizerANNXX.dat`
- `XX` is auto-incremented version number (`00` to `99`)
- File payload contains full ANN weights and bias blocks (W1, B1, W2, B2)

On completion, display returns to Main Menu.

For a concise operator checklist, see [Save ANN quick workflow](Speech_Recognition_Translator/QUICKSTART.md#save-ann-main-menu-page-2-option-7).

### Load Speech ANN (Main Menu Page 2, option 8)

- Entry point: **Main Menu Page 2 -> 8**
- Loads from saved files at `microsd/RecognizerANNXX.dat`
- Default selection is the **highest available version** (`XX` max)

Selection screen behavior:

- **Line 0:** `Load Speech ANN`
- **Line 1:** selected version (`ANN vXX`)
- **Line 2:** selection index (`current/total`)
- **Line 3:** `A/B:Sel #:Load *:Bk`

Key behavior before load starts:

- **A/B** = move selection up/down through available saved versions
- **`*`** = return to Main Menu without loading
- **`#`** = load selected ANN into **all 5** `Speech_Process_8bit_relu` devices

During load, LCD displays version and upload progress per device.

Upload behavior:

- Each target stage-2 device is paused/frozen while its ANN is written, then resumed
- Process repeats across all 5 stage-2 device addresses
- On completion, display returns to Main Menu

Capture lifecycle when **`#`** is pressed:

1. Neural network input is cleared.
2. System waits for speech (`peak > moving average threshold`).
3. Capture continues until speech ends (`peak <= moving average threshold`) or frame limit is reached.
4. Input is frozen, buffered data is saved, input is cleared, and processing resumes.

Captured training buffer size is **40 bytes × 100 frames** per word capture.

Training data file behavior:

- Saved path: `microsd/<username>/<word>.dat`
- Existing file for that word is overwritten by new capture.

## Stage‑2 FIFO Read Protocol

The Translator reads each stage‑2 device via I2C0 (master):

- `0x01` → FIFO length (16‑bit)
- `0x05` → FIFO entry (40‑bit):
  - Byte 1: neuron ID
  - Byte 2: max value
  - Byte 3: female value
  - Byte 4: male value
  - Byte 5: user ID (0 = unknown, 1-20 = user)

## Phoneme Buffering

When a silence packet is detected (SIL inter‑word or inter‑sentence), the translator checks a **15‑entry** phoneme buffer and attempts a dictionary match.

## Dictionary Storage (microSD)

The microSD card holds:

- **Weights/Bias** for stage‑2 devices
- **Translation dictionary**
- **UserList.txt** for user ID → name mapping

Dictionary entries:

- ID number
- phoneme sequence
- text word

Dictionary must be **sorted by phoneme order** for fast matching.

## Translation Output

Each translated word includes:

- Direction (beam index 0‑4)
- Gender tag (female/male)
- Confidence (from max/female/male values)

## microSD Capacity

For 256GB cards, ensure **SDHC/SDXC** support and format as **FAT32**. A FatFs‑based driver is recommended.

## Command Set (Stage‑2 Control)

The Translator issues commands to stage‑2 units for configuration:

- Load/Save **weights & biases** (bulk page mode)
- Set **target neuron** for training
- Freeze input / pause processing

## Build

```bash
cd Speech_Recognition_Translator
mkdir -p build
cd build
cmake ..
ninja
```

## Status

This module provides a working stage-3 translator pipeline with:

- Stage-2 FIFO reads over I2C
- 15-phoneme sequence buffering and silence-triggered lookup
- microSD FatFs dictionary storage
- Unknown-word capture (`NewWords.dat`)
- 20x4 LCD + keypad menu flow
- User profile selection via `UserList.txt`

## Multilingual Support & Unknown Word Tracking

The translator supports multiple languages through a language-ID system and tracks unrecognized phoneme sequences using a two-file dictionary design:

- **Dictionary.dat**: Main sorted dictionary (73 bytes/record, fixed-width text)
- **NewWords.dat**: Sequential unknown-word file (73 bytes/record, fixed-width text)
- **Language.dat**: Language ID ↔ name mapping (text records, 20 entries)

### Two-File Architecture

```txt
Dictionary.dat (Sorted)         NewWords.dat (Sequential)
├─ Maintains sort order         ├─ Unknown words appended
├─ Binary search capable        │  without sort overhead
├─ Stable, pre-validated data   └─ Linear search within file
└─ Primary lookup source           (typically <100 entries)

Lookup Flow:
1. Search Dictionary.dat
2. If not found, search NewWords.dat
3. If still not found, append to NewWords.dat
```

### Unknown Word Lifecycle

```txt
Phoneme Sequence Detected
    ↓
dict_lookup_word(seq) → NOT FOUND
    ↓
dict_add_unknown_word(seq)
    ├─ Generate label: UnRecognisedXX
    ├─ Set language_id = 0
    └─ Append to NewWords.dat
```

Note: the label text in files uses British spelling (`UnRecognisedXX`), while UI text may use `Unrecognized`.

### File Specifications

#### Language.dat

- **Purpose**: Language ID mapping
- **Format**: Text records (`HH LanguageName\r\n`)
  - Chars 0-1: Language ID (2-digit hex)
  - Char 2: Space separator
  - Chars 3..N: Language name
  - Line ending: CRLF
- **Entries**: 20 predefined languages
- **Size**: Variable (~235 bytes for 20 default entries)
- **Location**: `/microsd/Language.dat`

#### Dictionary.dat

- **Purpose**: Primary sorted lookup table
- **Format**: 73 bytes/record (fixed-width text)
  - Chars 0-44: 15 two-digit hex phoneme IDs separated by spaces (includes trailing space)
  - Chars 45-70: Word field (26 chars, space padded)
  - Chars 71-72: CRLF
- **Sort Order**: Lexicographic by phoneme sequence
- **Location**: `/microsd/Dictionary.dat`

#### NewWords.dat

- **Purpose**: Unknown-word accumulation during runtime
- **Format**: 73 bytes/record (same as Dictionary.dat)
- **Word Label**: `UnRecognised00` ... `UnRecognised99`
- **Location**: `/microsd/NewWords.dat`

### Language IDs

```c
#define LANG_UNKNOWN      0
#define LANG_ENGLISH      1
#define LANG_SPANISH      2
#define LANG_FRENCH       3
#define LANG_GERMAN       4
#define LANG_ITALIAN      5
#define LANG_PORTUGUESE   6
#define LANG_RUSSIAN      7
#define LANG_CHINESE      8
#define LANG_JAPANESE     9
#define LANG_KOREAN      10
#define LANG_ARABIC      11
#define LANG_HINDI       12
#define LANG_DUTCH       13
#define LANG_SWEDISH     14
#define LANG_TURKISH     15
#define LANG_POLISH      16
#define LANG_GREEK       17
#define LANG_HEBREW      18
#define LANG_VIETNAMESE  19
```

### API Reference

- `dict_lookup_word(const uint8_t *seq, char *word_out, size_t word_out_len)`
  - Searches `Dictionary.dat`, then `NewWords.dat`
- `dict_add_unknown_word(const uint8_t *seq)`
  - Appends unknown sequences into `NewWords.dat`
- `dict_merge_new_words(void)`
  - Appends `NewWords.dat` into `Dictionary.dat`, then deletes `NewWords.dat`
- `create_language_file(void)`
  - Creates `Language.dat` if missing, without overwriting existing data

### Usage Examples

Generate initial dictionary files:

```bash
python3 generate_dicts.py /path/to/microsd
```

Expected outputs:

- `/path/to/microsd/Language.dat` (text format, ~235 bytes with defaults)
- `/path/to/microsd/Dictionary.dat` (empty template)

### Constants

```c
#define DICT_HEX_FIELD_CHARS 45
#define DICT_WORD_SIZE 26
#define DICT_RECORD_SIZE 73
```

### Performance Notes

- Dictionary lookup: O(log n) binary search on sorted `Dictionary.dat`
- NewWords lookup: O(m), where m is unknown-word count
- Unknown append: O(1)
- Merge operation: O(k), where k is merge record count

### File Size Summary

| File                        | Count | Size   | Bytes/Entry |
|-----------------------------|-------|--------|-------------|
| Language.dat                | 20    | ~235 B | variable    |
| Dictionary.dat (1000 words) | 1000  | 73 KB  | 73          |
| NewWords.dat (100 words)    | 100   | 7.3 KB | 73          |
| **Total**                   | 1120  | ~80 KB | -           |

### Testing Checklist

- [ ] Language.dat created with 20 entries (text format)
- [ ] Dictionary.dat created (initially empty)
- [ ] Unknown word added to NewWords.dat
- [ ] Output includes `[NEW]` tag for unknown words
- [ ] Same unknown sequence recognized on second occurrence
- [ ] `dict_merge_new_words()` merges entries successfully
- [ ] NewWords.dat removed after merge

### Troubleshooting

| Problem              | Cause              | Solution                 |
|----------------------|--------------------|--------------------------|
| No Language.dat      | SD not ready       | Check SD mount status    |
| NewWords not created | SD write failed    | Verify write permissions |
| Words not found      | Phoneme mismatch   | Check phoneme sequence   |
| Merge fails          | Append error       | Free up SD space         |
| Performance slow     | NewWords too large | Run merge operation      |
