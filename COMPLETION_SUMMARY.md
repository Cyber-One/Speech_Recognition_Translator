# Speech Recognition Translator - Completion Summary

## Current Project State

The translator stage is implemented as a practical stage-3 module for the RP2040 speech pipeline.

It currently provides:

- Stage-2 FIFO intake from 5 devices (`0x60`-`0x64`)
- 15-phoneme sequence buffering and silence-triggered word lookup
- Dictionary lookup with sorted primary dictionary + unknown-word fallback
- User selection workflow through `UserList.txt`
- 20x4 LCD + keypad menu system for field use
- microSD persistence for dictionary and user/language metadata

## Menu System (What Users See)

- `#` from default screen opens menu
- Main Menu has two pages:
  - Page 0: `1 Add New User`, `2 User Menu`
  - Page 1: training and unrecognized-word actions
- Page switching behavior:
  - `B` moves page 0 → page 1
  - `A` moves page 1 → page 0
- `*` returns back/exit

### User Menu

- Shows `User ID: <id> <name>`
- `A/B` cycles configured users
- `#` selects user
- `*` returns to Main Menu

## microSD Data Model (Current)

### Language.dat

- Beginner-editable text file
- Format per line: `HH LanguageName` + CRLF
- Example:

```txt
00 Unknown
01 English
02 Spanish
```

### Dictionary.dat

- Sorted primary dictionary
- Fixed-width text record: 73 bytes
- Record layout:
  - 45 chars: `15` two-digit hex phoneme IDs with spaces
  - 26 chars: word field (space padded)
  - 2 chars: CRLF

### NewWords.dat

- Same 73-byte record format as `Dictionary.dat`
- Auto-created at runtime for unknown phoneme sequences
- Unknown labels use `UnRecognisedXX`

### UserList.txt

- User ID to name mapping used by menu and output context

## Lookup Flow

1. Search `Dictionary.dat` using binary search (sorted file).
2. If no match, search `NewWords.dat` sequentially.
3. If still no match, append new unknown entry to `NewWords.dat`.

## Wiring Summary (Translator Board)

### I2C0 (`GPIO 20/21`)

- Stage-2 processors: `0x60`-`0x64`
- LCD expander: `0x27`
- Keypad expander: `0x26`

### SPI0 (microSD)

- `18` = SCK
- `19` = MOSI
- `16` = MISO
- `17` = CS

### Other GPIO

- Word-ready inputs: `6-10`
- Fault outputs: `11-15`
- Mode select: `2-3`
- Optional TTL UART: `0-1`

## Beginner Notes

- Always verify wiring and shared ground first.
- Keep I2C addresses unique on the same bus.
- Keep dictionary sorted if you want binary search lookups to stay correct.
- Use text file formats during development so file issues are easy to inspect.

## Build Status

Current source builds successfully using:

```bash
cmake --build build
```

This summary reflects the current implementation and replaces older references to legacy binary dictionary/language layouts.
