# Speech Recognition Translator - Quick Start

This file is the single setup guide for firmware flashing, dictionary preparation, and first-run validation.

## 5-Minute Setup

### Step 1: Prepare microSD Card

1. Format the card as FAT32 (or exFAT for larger cards if supported by your deployment).
2. Create folder `/microsd/` on the card.
3. Copy required files into `/microsd/`:
   - `Language.dat` (640 bytes)
   - `Dictionary.dat` (your populated dictionary)
  - `UserList.txt` (user ID to name mapping; ID 0 is Unknown)
   - `PhonemeList.txt` (reference)

### Step 2: Generate/Prepare Dictionary Files

Use the utility script to generate `Language.dat` and an empty `Dictionary.dat` template:

```bash
python3 generate_dicts.py /path/to/microsd
```

Then populate `Dictionary.dat` records with your word mappings.

### Step 3: Flash Firmware

```bash
# Option A: picotool
picotool load build/Speech_Recognition_Translator.uf2 -fx

# Option B: OpenOCD
openocd -s scripts -f interface/cmsis-dap.cfg \
  -f target/rp2040.cfg \
  -c "program build/Speech_Recognition_Translator.uf2 verify reset exit"
```

### Step 4: Wire Hardware

- I2C0 (`SDA=20`, `SCL=21`) to five stage-2 processors (`0x60`–`0x64`)
- SPI0 (`18,19,16,17`) to microSD reader
- UART0 (`0,1`) optional serial output
- GPIO `2–3` mode select (optional)

### Step 5: Boot and Monitor

Open serial at 115200 baud and check startup:

```txt
Speech Recognition Translator starting...
Dictionary loaded successfully
```

## Dictionary File Format

`Dictionary.dat` record format is 42 bytes:

- Bytes `0-1`: language ID (little-endian `uint16`)
- Bytes `2-16`: phoneme sequence (15 bytes)
- Bytes `17-41`: word (25-byte null-padded ASCII)

`Dictionary.dat` must be sorted lexicographically by phoneme sequence.

`NewWords.dat` uses the same 42-byte record layout and is auto-created for unknown words.

## Language IDs (0-19)

```txt
0  = Unknown           10 = Korean
1  = English           11 = Arabic
2  = Spanish           12 = Hindi
3  = French            13 = Dutch
4  = German            14 = Swedish
5  = Italian           15 = Turkish
6  = Portuguese        16 = Polish
7  = Russian           17 = Greek
8  = Chinese           18 = Hebrew
9  = Japanese          19 = Vietnamese
```

## Example: Add Dictionary Records

```python
import struct

def add_record(filename, lang_id, phoneme_seq, word):
    record = bytearray(42)
    struct.pack_into('<H', record, 0, lang_id)
    record[2:17] = phoneme_seq
    word_bytes = word.encode('ascii')[:24]
    record[17:17+len(word_bytes)] = word_bytes
    with open(filename, 'ab') as f:
        f.write(record)

add_record('Dictionary.dat', 1, b'\x05\x10\x15\x20\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00', 'hello')
add_record('Dictionary.dat', 1, b'\x05\x10\x15\x21\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00', 'world')
```

## Runtime API Quick Reference

- `dict_lookup_word(seq, word_out, len)` searches `Dictionary.dat` then `NewWords.dat`
- `dict_add_unknown_word(seq)` appends unknown words into `NewWords.dat`
- `dict_merge_new_words()` merges `NewWords.dat` into `Dictionary.dat`
- `create_language_file()` ensures `Language.dat` exists
- `create_user_list_file()` ensures `UserList.txt` exists

## Troubleshooting

| Issue                          | Check                                                   |
|--------------------------------|---------------------------------------------------------|
| `f_mount` failed               | Verify SPI wiring and SD formatting                     |
| `f_open Dictionary.dat` failed | Confirm `/microsd/Dictionary.dat` exists                |
| Words not found                | Verify sequence encoding and sort order                 |
| NewWords grows too large       | Run `dict_merge_new_words()` periodically               |
| No output words                | Verify stage-2 devices, I2C pull-ups, word-ready lines  |

## Hardware Pinout

```txt
SPI0: 18=SCK, 19=MOSI, 16=MISO, 17=CS
I2C0: 20=SDA, 21=SCL
UART0: 0=TX, 1=RX
Mode select: 2-3
Word-ready inputs: 6-10
Fault outputs: 11-15
```

## References

- <https://www.sdcard.org/>
- <http://elm-chan.org/fsw/ff/>
- <https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf>
