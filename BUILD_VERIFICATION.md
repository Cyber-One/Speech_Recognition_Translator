# Speech Recognition Translator - Build & Runtime Verification

**Status:** ✅ Build succeeds and firmware is runnable with current documentation.

## Verified Build

Recent local build command:

```bash
cmake --build build
```

Result:

- `Speech_Recognition_Translator.elf` links successfully.
- No translator-side compile failures were observed in the latest run.

## What Is Verified in This Stage

This stage is the **word translator and control hub**:

- Reads stage-2 FIFOs from I2C addresses `0x60`-`0x64`
- Buffers 15-phoneme sequences per beam
- Triggers dictionary lookup when silence phonemes are detected
- Looks up words from `Dictionary.dat` (binary search)
- Falls back to `NewWords.dat` (sequential search)
- Captures unknown sequences as `UnRecognisedXX`
- Drives 20x4 LCD + keypad menu flow
- Supports user selection through `UserList.txt`

## Current microSD Files (Expected)

Inside `/microsd/`:

- `Language.dat` (text lines: `HH LanguageName`)
- `Dictionary.dat` (73-byte fixed-width text records, sorted)
- `NewWords.dat` (auto-created, same 73-byte format)
- `UserList.txt` (user ID to name mapping)
- `PhonemeList.txt` (reference list)

## File Format Checks

### Language.dat

- One language per line
- Format: `HH Name` + CRLF
- Example: `01 English`

### Dictionary.dat / NewWords.dat

- 73 bytes per record
- 45 chars: 15 phoneme bytes as hex with spaces
- 26 chars: word field (space padded)
- 2 chars: CRLF

## Wiring Verification (Beginner Checklist)

### I2C0 bus (GPIO 20/21)

Connected devices:

- Stage-2 processors: `0x60`-`0x64`
- LCD PCF8574: `0x27`
- Keypad PCF8574: `0x26`

Checklist:

- Common ground between all boards
- 3.3 V logic-compatible modules
- SDA/SCL pull-ups present
- No I2C address conflicts

### SPI0 microSD

- `GPIO 18` = SCK
- `GPIO 19` = MOSI
- `GPIO 16` = MISO
- `GPIO 17` = CS

### Other GPIO

- Word-ready inputs: `GPIO 6-10`
- Fault outputs: `GPIO 11-15`
- Output mode select: `GPIO 2-3`
- TTL UART (optional): `GPIO 0-1`

## Functional Smoke Test

1. Power system and open serial monitor at 115200.
2. Confirm startup messages and SD mount success.
3. Press `#` and verify menu pages (`Main Menu Pg 0/1`).
4. Enter `User Menu` (option 2), cycle users with `A/B`, select with `#`.
5. Feed phoneme data and confirm word output.
6. Confirm unknown sequences appear in `NewWords.dat`.

## Notes for Learners

- Keep documentation and firmware constants in sync after each wiring or format change.
- Prefer text file formats on microSD during early development because they are easier to inspect and fix.
- If behavior looks wrong, verify wiring first, then check file format correctness.
