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

This module currently provides the **hardware interface** and **FIFO ingest** scaffolding. Dictionary parsing, SD card I/O, and upstream I2C output are stubbed for implementation.

## Multilingual Support & Unknown Word Tracking

The translator supports multiple languages through a language-ID system and tracks unrecognized phoneme sequences using a two-file dictionary design:

- **Dictionary.dat**: Main sorted dictionary (42 bytes/record)
- **NewWords.dat**: Sequential unknown-word file (42 bytes/record)
- **Language.dat**: Language ID ↔ name mapping (32 bytes/record, 20 entries)

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

### File Specifications

#### Language.dat

- **Purpose**: Language ID mapping
- **Format**: 32 bytes/record
  - Bytes 0-1: Language ID (little-endian uint16)
  - Bytes 2-31: Language name (null-terminated, null-padded ASCII)
- **Entries**: 20 predefined languages
- **Size**: 640 bytes
- **Location**: `/microsd/Language.dat`

#### Dictionary.dat

- **Purpose**: Primary sorted lookup table
- **Format**: 42 bytes/record
  - Bytes 0-1: Language ID
  - Bytes 2-16: Phoneme sequence (15 bytes)
  - Bytes 17-41: Word (25 bytes, null-padded)
- **Sort Order**: Lexicographic by phoneme sequence
- **Location**: `/microsd/Dictionary.dat`

#### NewWords.dat

- **Purpose**: Unknown-word accumulation during runtime
- **Format**: 42 bytes/record (same as Dictionary.dat)
- **Language ID**: Always 0 (unknown)
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

- `/path/to/microsd/Language.dat` (640 bytes)
- `/path/to/microsd/Dictionary.dat` (empty template)

### Constants

```c
#define DICT_RECORD_SIZE 42
#define LANG_RECORD_SIZE 32
#define DICT_LANG_ID_SIZE 2
#define DICT_PHONEME_SIZE 15
#define DICT_WORD_SIZE 25
#define LANG_ID_SIZE 2
#define LANG_NAME_SIZE 30
```

### Performance Notes

- Dictionary lookup: O(n) with early-exit optimization on sorted data
- NewWords lookup: O(m), where m is unknown-word count
- Unknown append: O(1)
- Merge operation: O(k), where k is merge record count

### File Size Summary

| File                        | Count | Size   | Bytes/Entry |
|-----------------------------|-------|--------|-------------|
| Language.dat                | 20    | 640 B  | 32          |
| Dictionary.dat (1000 words) | 1000  | 42 KB  | 42          |
| NewWords.dat (100 words)    | 100   | 4.2 KB | 42          |
| **Total**                   | 1120  | ~47 KB | -           |

### Testing Checklist

- [ ] Language.dat created with 20 entries (640 bytes)
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
