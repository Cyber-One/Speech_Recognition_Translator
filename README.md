# Speech Recognition Translator

## Overview

This stage aggregates phoneme outputs from **five** Speech_Process_8bit_relu units, resolves phoneme sequences into text, and emits translated words with **gender** and **direction** tags. It also manages **microSD** storage for neural weights/bias and the translation dictionary.

- **I2C0 master** reads FIFO data from stage‑2 devices at **0x60–0x64**
- **GPIO inputs** (one per stage‑2) indicate valid FIFO data
- **SPI** microSD stores dictionary + stage‑2 weights/bias
- **Output mode** selectable via 2 GPIO pins (USB / TTL / I2C)
- **Diagnostic GPIOs** indicate stage‑2 device failure

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
- `0x05` → FIFO entry (32‑bit):
  - Byte 1: neuron ID
  - Byte 2: max value
  - Byte 3: female value
  - Byte 4: male value

## Phoneme Buffering

When a silence packet is detected (SIL inter‑word or inter‑sentence), the translator checks a **15‑entry** phoneme buffer and attempts a dictionary match.

## Dictionary Storage (microSD)

The microSD card holds:

- **Weights/Bias** for stage‑2 devices
- **Translation dictionary**

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
