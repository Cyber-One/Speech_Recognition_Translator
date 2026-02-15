#!/usr/bin/env python3
"""Generate Language.dat and editable fixed-width Dictionary.dat files.

Dictionary.dat text record format (fixed width):
    15 two-digit hex values plus trailing spaces (45 chars)
    + 2-digit hex language ID + space (3 chars)
    + word field padded with spaces to 26 chars
    + CRLF

Record length = 76 bytes, which keeps binary-search indexing simple.
"""

import sys
import os

# Record sizes
DICT_HEX_COUNT = 15
DICT_HEX_FIELD_CHARS = 45
DICT_LANG_ID_CHARS = 2
DICT_LANG_SEP_CHARS = 1
DICT_WORD_SIZE = 26
DICT_RECORD_SIZE = DICT_HEX_FIELD_CHARS + DICT_LANG_ID_CHARS + DICT_LANG_SEP_CHARS + DICT_WORD_SIZE + 2

DICT_PHONEME_SIZE = 15

# Language definitions
LANGUAGES = [
    (0, "Unknown"),
    (1, "English"),
    (2, "Spanish"),
    (3, "French"),
    (4, "German"),
    (5, "Italian"),
    (6, "Portuguese"),
    (7, "Russian"),
    (8, "Chinese"),
    (9, "Japanese"),
    (10, "Korean"),
    (11, "Arabic"),
    (12, "Hindi"),
    (13, "Dutch"),
    (14, "Swedish"),
    (15, "Turkish"),
    (16, "Polish"),
    (17, "Greek"),
    (18, "Hebrew"),
    (19, "Vietnamese"),
]


def create_language_dat(output_path):
    """Create Language.dat with language ID mappings."""
    print(f"Creating {output_path}...")
    
    with open(output_path, 'w', newline='') as f:
        for lang_id, lang_name in LANGUAGES:
            f.write(f"{lang_id:02X} {lang_name}\r\n")
            print(f"  {lang_id:3d}: {lang_name}")
    
    print(f"✓ Created {output_path} ({len(LANGUAGES)} languages)")
    return True


def create_dictionary_dat(input_file, output_path, language_id=0):
    """
    Create Dictionary.dat as an empty fixed-width text file.
    """
    print(f"Creating {output_path}...")
    
    # Create empty file
    open(output_path, 'wb').close()
    print(f"✓ Created empty {output_path} (ready for population)")
    print(f"  Record format: 15 hex bytes + 26-char word + CRLF")
    print(f"  Each record: {DICT_RECORD_SIZE} bytes")
    return True


def format_dict_record(phoneme_seq, word, language_id=0):
    """Build one fixed-width Dictionary.dat record (as bytes)."""
    if len(phoneme_seq) != DICT_PHONEME_SIZE:
        raise ValueError(f"Phoneme sequence must be {DICT_PHONEME_SIZE} bytes")

    if len(word) > DICT_WORD_SIZE:
        raise ValueError(f"Word too long (max {DICT_WORD_SIZE} chars)")

    if language_id < 0 or language_id > 255:
        raise ValueError("language_id must be 0..255")

    hex_part = ''.join(f"{b:02X} " for b in phoneme_seq)  # 45 chars with trailing space
    if len(hex_part) != DICT_HEX_FIELD_CHARS:
        raise ValueError("Internal format error: bad hex field length")

    word_part = word.ljust(DICT_WORD_SIZE, ' ')
    line = f"{hex_part}{language_id:02X} {word_part}\r\n"
    return line.encode('ascii')


def add_dictionary_entry(output_path, phoneme_seq, word, language_id=0):
    """
    Add a single entry to Dictionary.dat (append mode).
    
    Args:
        output_path: Path to Dictionary.dat
        phoneme_seq: List of 15 phoneme IDs (bytes)
        word: Word string (max 25 chars)
        language_id: Language ID (default 0 = Unknown)
    """
    if len(phoneme_seq) != DICT_PHONEME_SIZE:
        raise ValueError(f"Phoneme sequence must be {DICT_PHONEME_SIZE} bytes")
    
    record = format_dict_record(phoneme_seq, word, language_id=language_id)

    with open(output_path, 'ab') as f:
        f.write(record)


def get_language_name(language_id):
    """Get language name from ID."""
    for lid, name in LANGUAGES:
        if lid == language_id:
            return name
    return "Unknown"


def print_dictionary_entry(data):
    """Print a dictionary entry in human-readable format."""
    if len(data) != DICT_RECORD_SIZE:
        return None
    line = data.decode('ascii', errors='ignore')

    hex_part = line[:DICT_HEX_FIELD_CHARS]
    lang_hex = line[DICT_HEX_FIELD_CHARS:DICT_HEX_FIELD_CHARS + DICT_LANG_ID_CHARS]
    word = line[DICT_HEX_FIELD_CHARS + DICT_LANG_ID_CHARS + DICT_LANG_SEP_CHARS:
                DICT_HEX_FIELD_CHARS + DICT_LANG_ID_CHARS + DICT_LANG_SEP_CHARS + DICT_WORD_SIZE].rstrip(' ')
    phoneme_seq = hex_part.strip().split(' ')
    
    return {
        'phoneme_seq': ' '.join(phoneme_seq),
        'language_id': int(lang_hex, 16) if lang_hex.strip() else 0,
        'word': word
    }


def main():
    """Main entry point."""
    if len(sys.argv) < 2:
        print("Usage: python3 generate_dicts.py <output_directory> [--with-sample]")
        print("")
        print("Examples:")
        print("  python3 generate_dicts.py /path/to/microsd")
        print("  python3 generate_dicts.py . --with-sample")
        print("")
        print("This creates:")
        print("  - Language.dat (language ID to name mappings)")
        print("  - Dictionary.dat (empty template, ready for population)")
        sys.exit(1)
    
    output_dir = sys.argv[1]
    with_sample = '--with-sample' in sys.argv
    
    # Create output directory if needed
    os.makedirs(output_dir, exist_ok=True)
    
    lang_path = os.path.join(output_dir, "Language.dat")
    dict_path = os.path.join(output_dir, "Dictionary.dat")
    
    # Create Language.dat
    create_language_dat(lang_path)
    print()
    
    # Create Dictionary.dat
    create_dictionary_dat(None, dict_path)
    print()
    
    print("✓ File generation complete!")
    print("")
    print("File locations:")
    print(f"  Language.dat: {os.path.abspath(lang_path)}")
    print(f"  Dictionary.dat: {os.path.abspath(dict_path)}")
    print("")
    print("Next steps:")
    print("  1. Copy these files to your microSD card /microsd/ directory")
    print("  2. Populate Dictionary.dat with phoneme sequence → word mappings")
    print("  3. The system will automatically create NewWords.dat for unknown words")
    print("  4. Use dict_merge_new_words() to merge unknown words back into Dictionary.dat")
    print("")
    print("Record format reference:")
    print(f"  Language.dat: text records")
    print(f"    - 2 hex chars: Language ID (00-FF)")
    print(f"    - 1 char: Space separator")
    print(f"    - N chars: Language name")
    print(f"    - 2 chars: CRLF")
    print(f"  Dictionary.dat: {DICT_RECORD_SIZE} bytes per record")
    print(f"    - 45 chars: 15 hex phoneme IDs (00-FF) separated by spaces")
    print(f"    - 2 chars: Language ID in hex (00-FF)")
    print(f"    - 1 char: Space separator")
    print(f"    - 26 chars: Word (space padded)")
    print(f"    - 2 chars: CRLF")


if __name__ == '__main__':
    main()
