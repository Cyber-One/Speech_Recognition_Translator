# Multilingual Support & Unknown Word Tracking

## Overview

The Speech Recognition Translator now supports multiple languages through a language ID system. It automatically tracks unrecognized phoneme sequences in a separate file, maintaining performance through a **two-file architecture**:

- **Dictionary.dat** - Sorted main dictionary (42 bytes/record, supports fast lookup)
- **NewWords.dat** - Sequential file for unknown words (42 bytes/record, created on first use)
- **Language.dat** - Language ID ↔ Name mappings (32 bytes/record, 20 languages = 640 bytes)

## Two-File Architecture

### Why Two Files?

```
Dictionary.dat (Sorted)         NewWords.dat (Sequential)
├─ Maintains sort order         ├─ Unknown words appended
├─ Binary search capable        │  without sort overhead
├─ Stable, pre-validated data   └─ Linear search within file
└─ Primary lookup source           (typically <100 entries)

Lookup Performance:
1. Search Dictionary.dat O(n) with early exit on sorted data
2. If not found, search NewWords.dat O(m) where m << n
3. If not found, append to NewWords.dat O(1)
```

### Search Algorithm

```c
dict_lookup_word(phoneme_seq, word_out):
    // Step 1: Search sorted Dictionary.dat
    for each record in Dictionary.dat:
        if sequence matches:
            return word  ✓ FOUND
        if sequence > current_record's sequence:
            break  // Not in Dictionary.dat (sorted)
    
    // Step 2: Search NewWords.dat (if it exists)
    for each record in NewWords.dat:
        if sequence matches:
            return word  ✓ FOUND
    
    // Step 3: Not found in either file
    return false
```

### Unknown Word Lifecycle

```
Phoneme Sequence Detected:
    ↓
    dict_lookup_word(seq) → NOT FOUND
    ↓
    dict_add_unknown_word(seq)
    ├─ Generate label: UnRecognisedXX
    ├─ Create record with language_id=0
    └─ Append to NewWords.dat
    ↓
    Output: "word=UnRecognised00 [NEW]"
    ↓
    (Later) dict_merge_new_words()
    ├─ Append all NewWords.dat to Dictionary.dat
    └─ Delete NewWords.dat
```

## File Specifications

### Language.dat

**Purpose:** Maps language IDs to language names

**Format:** 32 bytes per record
- Bytes 0-1: Language ID (little-endian uint16)
- Bytes 2-31: Language name (null-terminated, null-padded ASCII)

**Contents:** 20 predefined languages
```
ID | Language
---+----------
0  | Unknown
1  | English
2  | Spanish
3  | French
... (17 more)
```

**Size:** 640 bytes total (20 records × 32 bytes)
**Status:** Auto-generated on first boot, never overwritten
**Location:** `/microsd/Language.dat`

### Dictionary.dat

**Purpose:** Primary word lookup table (pre-validated, sorted)

**Format:** 42 bytes per record
- Bytes 0-1: Language ID (little-endian uint16)
- Bytes 2-16: Phoneme sequence (15 bytes, phoneme IDs 0x05-0x2C)
- Bytes 17-41: Word (null-terminated ASCII, null-padded to 25 bytes)

**Sort Order:** Lexicographically sorted by phoneme sequence (bytes 2-16)

**Status:** User-populated via import tools
**Location:** `/microsd/Dictionary.dat`

**Size:** Depends on vocabulary
- Example: 1000 words = 42 KB
- Example: 10000 words = 420 KB

### NewWords.dat

**Purpose:** Accumulates unknown words during operation (sequential append)

**Format:** 42 bytes per record (identical to Dictionary.dat)
- Bytes 0-1: Always 0 (language_id = UNKNOWN)
- Bytes 2-16: Phoneme sequence (15 bytes)
- Bytes 17-41: Word label (UnRecognised00, UnRecognised01, ..., UnRecognised99)

**Sort Order:** None (sequential append)

**Status:** Auto-created on first unknown word
**Location:** `/microsd/NewWords.dat`

**Lifecycle:**
1. Created automatically when first unknown word detected
2. Grows as unknown words accumulate
3. Merged into Dictionary.dat via `dict_merge_new_words()`
4. Deleted after successful merge

## Language IDs

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

## API Reference

### `dict_lookup_word()`

```c
static bool dict_lookup_word(const uint8_t *seq, char *word_out, size_t word_out_len);
```

Search for a phoneme sequence in Dictionary.dat and NewWords.dat.

**Parameters:**
- `seq`: 15-byte phoneme sequence
- `word_out`: Output buffer for word string
- `word_out_len`: Size of output buffer

**Returns:** `true` if found, `false` otherwise

**Algorithm:**
1. Rewind Dictionary.dat to start
2. Linear scan with early exit when sequence > current (sorted)
3. If not found, open and search NewWords.dat sequentially
4. Close NewWords.dat if opened

### `dict_add_unknown_word()`

```c
static bool dict_add_unknown_word(const uint8_t *seq);
```

Add a new unknown word to NewWords.dat.

**Parameters:**
- `seq`: 15-byte phoneme sequence

**Returns:** `true` on success, `false` on SD error

**Behavior:**
1. Open NewWords.dat in append mode (create if needed)
2. Create 42-byte record:
   - Language ID = 0 (Unknown)
   - Phoneme sequence = input seq
   - Word = UnRecognisedXX (auto-incremented)
3. Write record
4. Close file
5. Increment global counter `unrecognised_counter`

### `dict_merge_new_words()`

```c
static bool dict_merge_new_words(void);
```

Merge NewWords.dat into Dictionary.dat and delete NewWords.dat.

**Returns:** `true` on success, `false` on SD error

**Algorithm:**
1. Check if NewWords.dat exists (return true if not)
2. Open Dictionary.dat in append mode
3. Open NewWords.dat for reading
4. Copy all records from NewWords to Dictionary
5. Close both files
6. Delete NewWords.dat
7. Print merge statistics

**Important:** Should only be called when Dictionary.dat is not being searched

### `create_language_file()`

```c
static bool create_language_file(void);
```

Create Language.dat with predefined language mappings.

**Returns:** `true` on success, `false` on error

**Behavior:**
1. Check if Language.dat exists → return true (don't overwrite)
2. Create Language.dat for writing
3. Write 20 language records
4. Close file

**Note:** Called automatically during `dict_init()`

## Usage Examples

### File Generation

**Option 1: Python script**
```bash
python3 generate_dicts.py /path/to/microsd
```
Creates:
- `/path/to/microsd/Language.dat` (640 bytes)
- `/path/to/microsd/Dictionary.dat` (empty, 0 bytes)

**Option 2: Automatic on boot**
Firmware automatically creates Language.dat if missing.

### Adding Words to Dictionary

Users need to populate Dictionary.dat using external tools:
1. Create phoneme-to-word mappings (15 bytes phoneme ID + word)
2. Sort by phoneme sequence (critical for binary search optimization)
3. Encode as 42-byte records:
   - 2 bytes: language_id (1 for English, etc.)
   - 15 bytes: phoneme sequence
   - 25 bytes: word (null-padded)
4. Write to Dictionary.dat in sorted order

### Handling Unknown Words

System automatically tracks unknown words:

```
Input: Phoneme sequence [0x05, 0x10, 0x15, 0x20, ...]
↓
Lookup fails in both Dictionary.dat and NewWords.dat
↓
Auto-create record with language_id=0
↓
Auto-assign label "UnRecognised00"
↓
Append to NewWords.dat
↓
Output: "word=UnRecognised00 gender=male conf=185 [NEW]"
```

Later, merge unknown words back into main dictionary:
```c
if (dict_merge_new_words()) {
    printf("Merged new words into Dictionary.dat\n");
    // NewWords.dat now deleted
}
```

### Periodic Maintenance

After running for a while:
```
NewWords.dat status: 500 entries (21 KB)
↓
Run: dict_merge_new_words()
↓
Result: All 500 entries appended to Dictionary.dat
        NewWords.dat deleted
↓
Next unknown word will create new empty NewWords.dat
```

## Constants & Macros

```c
// Record sizes
#define DICT_RECORD_SIZE 42
#define LANG_RECORD_SIZE 32

// Dictionary field sizes
#define DICT_LANG_ID_SIZE 2
#define DICT_PHONEME_SIZE 15
#define DICT_WORD_SIZE 25

// Language file field sizes
#define LANG_ID_SIZE 2
#define LANG_NAME_SIZE 30

// Language IDs
#define LANG_UNKNOWN 0
#define LANG_ENGLISH 1
// ... see Language IDs section above
```

## Performance Characteristics

### Dictionary.dat Lookup

**Time Complexity:** O(n) with early exit
- n = number of entries in Dictionary.dat
- Typical: 1-2 ms for 1000-word dictionary

**Optimization:** Currently early exit when sequence > current record (sorted)
**Future:** Binary search possible (O(log n)) with minimal changes

### NewWords.dat Lookup

**Time Complexity:** O(m) where m = number of unknown words
- Typical: <1 ms (usually <100 entries)
- Rarely accessed (usually just appends)

### Unknown Word Addition

**Time Complexity:** O(1) append
- Typical: <1 ms (single 42-byte write)

### Merge Operation

**Time Complexity:** O(k) where k = unknown word count
- k × 42 bytes copied
- Typical: ~1 ms per 100 entries

## File Sizes Summary

| File | Count | Size | Bytes/Entry |
|------|-------|------|------------|
| Language.dat | 20 | 640 B | 32 |
| Dictionary.dat (1000 words) | 1000 | 42 KB | 42 |
| NewWords.dat (100 words) | 100 | 4.2 KB | 42 |
| **Total** | 1120 | ~47 KB | - |

## Build Information

**Firmware Size:** 163,328 bytes (159 KB)  
**Format Version:** 2.1 (with NewWords.dat support)  
**SDK:** Pico SDK 2.2.0  
**FatFs:** Configured with `FF_USE_STRFUNC=1`

## Testing Checklist

- [ ] Language.dat created with 20 entries (640 bytes)
- [ ] Dictionary.dat created (initially empty)
- [ ] Unknown word detected and added to NewWords.dat
- [ ] Output shows `[NEW]` tag for unknown words
- [ ] Same unknown word sequence recognized on second occurrence
- [ ] `dict_merge_new_words()` successfully merges entries
- [ ] NewWords.dat deleted after merge
- [ ] Dictionary.dat increased by merged entry count × 42

## Troubleshooting

| Problem | Cause | Solution |
|---------|-------|----------|
| No Language.dat | SD not ready | Check SD mount status |
| NewWords not created | SD write failed | Verify write permissions |
| Words not found | Phoneme mismatch | Check phoneme sequence |
| Merge fails | Append error | Free up SD space |
| Performance slow | NewWords too large | Run merge operation |

## Code Locations

**Main implementation:** `speech_recognition_translator.c`
- Lines 354-381: `create_language_file()`
- Lines 383-436: `dict_add_unknown_word()`
- Lines 438-524: `dict_lookup_word()` with two-file search
- Lines 526-580: `dict_merge_new_words()`
- Lines 1284-1316: `handle_stage2_entry()` integration

**File generation tool:** `generate_dicts.py`
- Cross-platform Python utility for initial setup

**FatFs config:** `third_party/fatfs/source/ffconf.h`
- Line 28: `FF_USE_STRFUNC=1` (required for f_gets/f_write)

## Future Enhancements

1. **Binary search:** O(log n) lookup in Dictionary.dat
2. **Auto-merge:** Automatic NewWords.dat merge when threshold reached
3. **Statistics:** Track recognition rates per language/word
4. **Language detection:** Infer language from phoneme patterns
5. **Compression:** Reduce storage with phoneme ID compression
6. **Indexing:** Optional index file for faster lookups
