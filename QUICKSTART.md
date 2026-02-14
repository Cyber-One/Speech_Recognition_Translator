# Speech Recognition Translator - Quick Start

## 5-Minute Setup

### Step 1: Prepare microSD Card

1. **Format:** microSD card to FAT32 (or exFAT for >32 GB)
2. **Create folder:** `/microsd/` on the card
3. **Copy files:**
   - `Dictionary.dat` (5.3 MB) → `/microsd/`
   - `PhonemeList.txt` (1 KB) → `/microsd/`

### Step 2: Flash

```bash
# Option A: Using picotool
picotool load build/Speech_Recognition_Translator.uf2 -fx

# Option B: Using OpenOCD (if debugger connected)
openocd -s scripts -f interface/cmsis-dap.cfg \
  -f target/rp2040.cfg \
  -c "program build/Speech_Recognition_Translator.uf2 verify reset exit"
```

### Step 3: Wire Hardware

Connect Pico to:

- **I2C0 (SDA=20, SCL=21):** To 5× stage-2 phoneme processors (0x60–0x64)
- **SPI0 (18,19,16,17):** To microSD card reader
- **UART0 (0,1):** To serial terminal (optional)
- **GPIO 2–3:** Output mode selector (optional)

### Step 4: Connect microSD

Insert microSD card into SPI reader connected to Pico SPI0 pins.

### Step 5: Monitor Output

```bash
# USB serial console (115200 baud)
screen /dev/ttyACM0 115200
# or PuTTY/minicom on Windows

# Expected startup sequence:
# Speech Recognition Translator starting...
# Dictionary loaded successfully
```

## Testing Without Audio Hardware

### Minimal Test (GPIO only)

```bash
# Flash & monitor only - verify SPI/SD detection
# Check serial output for "Dictionary loaded successfully"
```

### Full Test (with audio pipeline)

```bash
# 1. Ensure stage-1 audio capture is running
# 2. Ensure stage-2 phoneme processors are responding on I2C
# 3. Speak clearly (single words work best)
# 4. Observe word translations on serial monitor
```

## File Format Reference

### PhonemeList.txt (Reference Only)

```txt
0x02 SIL (silence inter-word)
0x05 AA  ɑ
0x06 AE  æ
...
0x2C AX  ə
```

### Dictionary.dat (Binary)

```txt
Record 0: [phoneme_seq[0-14]] [word[0-24]]
Record 1: [phoneme_seq[0-14]] [word[0-24]]
...
```

- 40 bytes per record
- ~67,000 records total
- Sorted by phoneme sequence (little-endian)

## Troubleshooting

### Serial Monitor Shows "f_mount failed"

- ❌ SD card not detected
- ✅ Check SPI wiring (pins 16–19, 17)
- ✅ Reformat card to FAT32
- ✅ Try different SD card reader (some require pull-ups)

### Serial Monitor Shows "f_open Dictionary.dat failed"

- ❌ File not found
- ✅ Verify `/microsd/Dictionary.dat` exists
- ✅ Check file spelling (case-sensitive on Linux)
- ✅ Copy file size should be ~5.3 MB

### No Words Output (But Dictionary Loaded)

- ❌ Audio pipeline not connected or responding
- ✅ Verify stage-1 and stage-2 devices are powered
- ✅ Check I2C wiring and pull-ups on SDA/SCL
- ✅ Monitor GPIO 11–15 (fault indicators) on oscilloscope

### Slow Word Lookups (>1 second)

- ❌ SD card latency high
- ✅ Expected behavior (linear search ~250 ms)
- ✅ Use binary search optimization (future enhancement)
- ✅ Implement caching (future enhancement)

## Hardware Pinout Diagram

```txt
Pico GPIO           Function
─────────────────────────────
SPI0:
  18 → SCK          SD card clock
  19 → MOSI         Data to SD
  16 ← MISO         Data from SD
  17 → CS           Chip select

I2C0:
  20 ← SDA          Data (pulls up)
  21 ← SCL          Clock (pulls up)

UART0 (TTL serial):
  0  → TX           9600–115200 baud
  1  ← RX

GPIO Inputs (mode select):
  2  ← MODE_SEL0    0 = USB, 1 = TTL, 2 = I2C
  3  ← MODE_SEL1

GPIO Inputs (word-ready flags):
  6–10 ← word_ready[5]  From stage-2 (active low)

GPIO Outputs (diagnostics):
  11–15 → stage2_fault[5]  SD/I2C failures per beam
```

## Expected Output Example

```txt
Speech Recognition Translator starting...
Dictionary loaded successfully
beam=0 word=HELLO gender=female conf=240
beam=2 word=WORLD gender=male conf=220
beam=1 word=GOODBYE gender=female conf=235
```

## Links & References

[SD Card Spec:](https://www.sdcard.org/)
[FatFs Documentation:](http://elm-chan.org/fsw/ff/)
[RP2040 Datasheet:](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf)

---

**Ready to go!** Flash the firmware and insert the microSD card. Should be up and running in seconds.
