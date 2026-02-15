# Speech Recognition Translator - Quick Start

This file is the single setup guide for firmware flashing, dictionary preparation, and first-run validation.

## 5-Minute Setup

### Step 1: Prepare microSD Card

1. Format the card as FAT32 (or exFAT for larger cards if supported by your deployment).
2. Create folder `/microsd/` on the card.
3. Copy required files into `/microsd/`:

- `Language.dat` (text format, e.g. `01 English`)
- `Dictionary.dat` (your populated dictionary)
- `UserList.txt` (user ID to name mapping; ID `0` is Unknown)
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
- I2C0 (`SDA=20`, `SCL=21`) also to:
  - LCD PCF8574 backpack at `0x27`
  - Keypad PCF8574 expander at `0x26`
- SPI0 (`18,19,16,17`) to microSD reader
- UART0 (`0,1`) optional serial output
- GPIO `2–3` mode select (optional)

Beginner wiring checklist:

- All devices share a common GND.
- Use 3.3 V logic-compatible modules.
- Ensure SDA/SCL pull-ups are present (module or external).
- Confirm I2C addresses do not conflict (`0x26`, `0x27`, `0x60`–`0x64`).

### Step 5: Boot and Monitor

Open serial at 115200 baud and check startup:

```txt
Speech Recognition Translator starting...
Dictionary loaded successfully
```

## 20x4 LCD + Keypad Controls

### Default Screen

- Line 0: status and microSD errors
- Lines 1-3: rolling history of last 10 recognized words (wrapped)

### Open Menu

- Press `#` from default screen

### Key Mapping

- `A` = Up
- `B` = Down
- `C` = Left
- `D` = Right

### Main Menu (Paged)

- Line 0 shows `Main Menu Pg 0`, `Main Menu Pg 1`, or `Main Menu Pg 2`
- Page 0:
  1. Add New User
  2. User Menu
  3. Start Training
- Page 1:
  4. Select Word from Unrecognized List
  5. Initiate Speech Generator Training
  6. Stage 2 ANN Training
- Page 2:
  7. Save ANN
  8. Load Speech ANN

Navigation:

- On page 0, `A/C/D` have no effect
- On page 0, `B` moves to page 1
- On page 1, `A` moves to page 0, `B` moves to page 2
- On page 2, `A` moves to page 1

### User Menu (from Main Menu option 2)

- Line 0 shows `User Menu`
- Line 1 shows `User ID: <id> <name>`
- Press `A/B` to cycle through configured users only (unassigned/default users excluded)
- Press `#` to select the displayed user
- Press `*` to return to Main Menu

Press `*` to back out to the default screen from menu screens.

### Training Menu (from Main Menu option 3)

Training opens only when a user is already selected.

- Press `3` from **Main Menu Page 0** to enter training
- If no user is selected, training does not open and status is set to `select user`

Display behavior:

- **Line 0:** `Training Memnu X/Y` (firmware text), where `X` is current word index and `Y` is total words
- **Line 1:** Current training word (centered)
- **Line 2:** `Recording: Present` or `Recording: Missing`
- **Line 3 (idle):** `A/B:Scroll #:Train`
- **Line 3 (armed/capturing):** `Speak When Ready` (centered)

Key behavior:

- `A` or `C` = previous word
- `B` or `D` = next word
- `#` = start capture for current word
- `*` = abort and return to **Main Menu Page 1**

### Stage 2 ANN Training (Main Menu Page 1, option 6)

- Entry: **Page 1 -> 6**
- Confirmation screen is required before activation:
  - Line 0: `Stage 2 ANN Train`
  - Line 1: `Are you sure?`
  - `#` starts ANN training
  - `*` cancels and returns to main menu

During training, the display shows the current word, the last result, and a running count.
Each word is replayed and retrained in epochs until target certainty reaches **>=80%** (or max epoch limit), using stage-2 telemetry feedback.
When complete, the UI returns to the main menu.

ANN training logs are appended on microSD at:

- `microsd/logs/<username>_ann_train.log`

### Speech Generator Training (Main Menu Page 1, option 5)

- Entry: **Page 1 -> 5**
- Trains Stage 4 (`Speech_Generation`, `0x65`) to produce phoneme images that Stage 2 channel 2 matches.

Flow:

1. Send one phoneme to Stage 4 and generate a `40 x 100` image.
2. Capture the image and replay lines into Stage 2 channel 2.
3. Read Stage-2 target confidence.
4. If confidence is below **80%**, run one Stage-4 backprop step and repeat.

Pass condition per phoneme: Stage-2 target confidence reaches **>=80%**.

### Save ANN (Main Menu Page 2, option 7)

- Entry: **Page 2 -> 7**
- Scope: save/export is from **Stage-2 channel/unit 2 only**
- Confirmation screen is required before save:
  - Line 0: `Save ANN`
  - Line 1: `Are you sure?`
  - `#` starts save
  - `*` cancels and returns to main menu

During save, LCD shows:

- New ANN version (`vXX`)
- Current phase (`Preparing`, `Read W1`, `Read B1`, `Read W2`, `Read B2`, `Saved`)
- Save progress percentage

Saved file behavior:

- Path format: `microsd/RecognizerANNXX.dat`
- `XX` is auto-incremented from `00` to `99`
- File includes complete ANN weights and bias blocks (W1, B1, W2, B2)

On completion, display returns to the main menu.

### Load Speech ANN (Main Menu Page 2, option 8)

- Entry: **Page 2 -> 8**
- Source files: `microsd/RecognizerANNXX.dat`
- Default selected file is the highest available ANN version

Selection screen:

- Line 0: `Load Speech ANN`
- Line 1: selected version (`ANN vXX`)
- Line 2: position (`current/total`)
- Line 3: `A/B:Sel #:Load *:Bk`

Before upload starts:

- `A/B` changes selected ANN version
- `*` cancels and returns to main menu
- `#` loads selected ANN to all 5 `Speech_Process_8bit_relu` devices

During upload, LCD shows device-by-device progress and percentage.
Each device ANN is paused/frozen during its write, then resumed when done.
After completion, the UI returns to main menu.

Capture lifecycle when `#` is pressed:

1. Neural-network input buffer is cleared.
2. System waits for speech start (`peak > moving average threshold`).
3. Capture continues until speech end (`peak <= moving average threshold`) or frame limit.
4. Input is frozen, buffered data is saved, input is cleared, and processing resumes.

Captured training buffer size is **40 bytes × 100 frames** per word capture.

Training file behavior:

- Save path: `microsd/<username>/<word>.dat`
- Existing file for the same word is overwritten.

## Dictionary File Format

`Dictionary.dat` record format is fixed-width text, 73 bytes per line:

- 15 two-character hex values separated by spaces with a trailing space (45 chars)
- Word field padded with spaces to 26 chars
- CRLF line ending (2 chars)

Example line:

```txt
05 10 15 20 00 00 00 00 00 00 00 00 00 00 00 hello[space padded to 26 chars]
```

`Dictionary.dat` must be sorted lexicographically by phoneme sequence.

`NewWords.dat` uses the same 73-byte fixed-width text layout and is auto-created for unknown words.
The runtime label prefix for unknown entries is `UnRecognisedXX`.

## Language File Format

`Language.dat` is plain text for easy editing:

- Format per line: `HH LanguageName` + CRLF
- `HH` is a two-digit hexadecimal language ID (for example `01` = English)

Example:

```txt
00 Unknown
01 English
02 Spanish
```

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
def add_record(filename, phoneme_seq, word):
  hex_part = ''.join(f"{b:02X} " for b in phoneme_seq)   # 45 chars
  word_part = word[:26].ljust(26, ' ')
  line = f"{hex_part}{word_part}\r\n"
  with open(filename, 'ab') as f:
    f.write(line.encode('ascii'))

add_record('Dictionary.dat', [0x05,0x10,0x15,0x20,0,0,0,0,0,0,0,0,0,0,0], 'hello')
add_record('Dictionary.dat', [0x05,0x10,0x15,0x21,0,0,0,0,0,0,0,0,0,0,0], 'world')
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
