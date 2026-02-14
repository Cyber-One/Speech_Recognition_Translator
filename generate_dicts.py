#!/usr/bin/env python3
"""
Generate Dictionary.dat and Language.dat files for Speech Recognition Translator.

This script creates the microSD card data files needed by the system:
- Language.dat: Mapping of language IDs to language names
- Dictionary.dat: Sorted phoneme sequence to word mappings

File Formats:
- Language.dat: 32 bytes per record (2-byte ID + 30-byte name)
- Dictionary.dat: 42 bytes per record (2-byte lang ID + 15-byte phoneme seq + 25-byte word)
"""

import struct
import sys
import os

# Record sizes
LANG_RECORD_SIZE = 32
DICT_RECORD_SIZE = 42

# Field sizes
LANG_ID_SIZE = 2
LANG_NAME_SIZE = 30

DICT_LANG_ID_SIZE = 2
DICT_PHONEME_SIZE = 15
DICT_WORD_SIZE = 25

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
    
    with open(output_path, 'wb') as f:
        for lang_id, lang_name in LANGUAGES:
            record = bytearray(LANG_RECORD_SIZE)
            
            # Write language ID (little-endian, 2 bytes)
            struct.pack_into('<H', record, 0, lang_id)
            
            # Write language name (null-padded)
            name_bytes = lang_name.encode('ascii')[:LANG_NAME_SIZE-1]
            record[LANG_ID_SIZE:LANG_ID_SIZE + len(name_bytes)] = name_bytes
            
            f.write(record)
            print(f"  {lang_id:3d}: {lang_name}")
    
    print(f"✓ Created {output_path} ({len(LANGUAGES)} languages)")
    return True


def create_dictionary_dat(input_file, output_path, language_id=1):
    """
    Create Dictionary.dat from a word list file.
    
    Input file format: One word per line, or "phoneme_seq word" format
    Assumes phoneme sequences are converted to 15-byte sequences.
    
    For now, creates an empty template that can be populated with real data.
    """
    print(f"Creating {output_path}...")
    
    # For initial generation, create an empty/template dictionary
    # Users should populate this with their own word/phoneme data
    
    with open(output_path, 'wb') as f:
        # Write a template record as an example
        record = bytearray(DICT_RECORD_SIZE)
        
        # Language ID (little-endian)
        struct.pack_into('<H', record, 0, language_id)
        
        # Phoneme sequence (15 bytes, all zeros for template)
        # In real usage, these would be phoneme IDs (0x05-0x2C)
        
        # Word (25 bytes, null-padded)
        word = "TEMPLATE"
        word_bytes = word.encode('ascii')[:DICT_WORD_SIZE-1]
        record[DICT_LANG_ID_SIZE + DICT_PHONEME_SIZE:
               DICT_LANG_ID_SIZE + DICT_PHONEME_SIZE + len(word_bytes)] = word_bytes
        
        # Note: Don't write the template, start with empty file
        # Users will populate via external tools or import
    
    # Create empty file
    open(output_path, 'wb').close()
    print(f"✓ Created empty {output_path} (ready for population)")
    print(f"  Record format: 2-byte language ID + 15-byte phoneme sequence + 25-byte word")
    print(f"  Each record: {DICT_RECORD_SIZE} bytes")
    return True


def add_dictionary_entry(output_path, phoneme_seq, word, language_id=1):
    """
    Add a single entry to Dictionary.dat (append mode).
    
    Args:
        output_path: Path to Dictionary.dat
        phoneme_seq: List of 15 phoneme IDs (bytes)
        word: Word string (max 25 chars)
        language_id: Language ID (default 1 = English)
    """
    if len(phoneme_seq) != DICT_PHONEME_SIZE:
        raise ValueError(f"Phoneme sequence must be {DICT_PHONEME_SIZE} bytes")
    
    if len(word) > DICT_WORD_SIZE - 1:
        raise ValueError(f"Word too long (max {DICT_WORD_SIZE-1} chars)")
    
    record = bytearray(DICT_RECORD_SIZE)
    
    # Language ID
    struct.pack_into('<H', record, 0, language_id)
    
    # Phoneme sequence
    record[DICT_LANG_ID_SIZE:DICT_LANG_ID_SIZE + DICT_PHONEME_SIZE] = bytes(phoneme_seq)
    
    # Word
    word_bytes = word.encode('ascii')
    record[DICT_LANG_ID_SIZE + DICT_PHONEME_SIZE:
           DICT_LANG_ID_SIZE + DICT_PHONEME_SIZE + len(word_bytes)] = word_bytes
    
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
    
    # Extract fields
    lang_id = struct.unpack_from('<H', data, 0)[0]
    phoneme_seq = data[DICT_LANG_ID_SIZE:DICT_LANG_ID_SIZE + DICT_PHONEME_SIZE]
    word_bytes = data[DICT_LANG_ID_SIZE + DICT_PHONEME_SIZE:
                       DICT_LANG_ID_SIZE + DICT_PHONEME_SIZE + DICT_WORD_SIZE]
    
    # Extract null-terminated string
    word = word_bytes.split(b'\0')[0].decode('ascii', errors='ignore')
    
    return {
        'lang_id': lang_id,
        'lang_name': get_language_name(lang_id),
        'phoneme_seq': ' '.join(f'0x{b:02X}' for b in phoneme_seq),
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
    print(f"  Language.dat: {LANG_RECORD_SIZE} bytes per record")
    print(f"    - 2 bytes: Language ID (little-endian)")
    print(f"    - 30 bytes: Language name (null-padded)")
    print(f"  Dictionary.dat: {DICT_RECORD_SIZE} bytes per record")
    print(f"    - 2 bytes: Language ID (little-endian)")
    print(f"    - 15 bytes: Phoneme sequence (phoneme IDs)")
    print(f"    - 25 bytes: Word (null-padded)")


if __name__ == '__main__':
    main()
