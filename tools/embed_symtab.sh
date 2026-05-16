#!/bin/bash
# tools/embed_symtab.sh
#
# Two-pass kernel build helper.
#
# Usage:
#   embed_symtab.sh <pass1_elf> <symtab_o> <strtab_o>
#
# Extracts .symtab and .strtab from <pass1_elf>, converts them to
# relocatable .o files with the section names .symtab_blob and .strtab_blob,
# and renames the boundary symbols to __symtab_start/__symtab_end and
# __strtab_start/__strtab_end so panic.c can find them.
#
# The generated .o files are then linked into the final kernel image.

set -e

PASS1_ELF="$1"
SYMTAB_O="$2"
STRTAB_O="$3"

if [ -z "$PASS1_ELF" ] || [ -z "$SYMTAB_O" ] || [ -z "$STRTAB_O" ]; then
    echo "Usage: $0 <pass1_elf> <symtab.o> <strtab.o>" >&2
    exit 1
fi

TMPDIR_WORK="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_WORK"' EXIT

SYMTAB_BIN="$TMPDIR_WORK/symtab.bin"
STRTAB_BIN="$TMPDIR_WORK/strtab.bin"

# Extract raw section data using the Python helper
python3 "$(dirname "$0")/extract_symtab.py" "$PASS1_ELF" "$SYMTAB_BIN" "$STRTAB_BIN"

# Handle empty symtab (no symbols — write empty .o files)
if [ ! -s "$SYMTAB_BIN" ]; then
    echo "[embed_symtab] WARNING: empty .symtab — creating stub objects" >&2
    # Create minimal stub .o with the expected symbols defined as zero-size
    printf '' > "$SYMTAB_BIN"
    printf '' > "$STRTAB_BIN"
fi

# ── .symtab blob ──────────────────────────────────────────────────────────────
# Convert binary → ELF32 relocatable, rename section, rename symbols
SYMTAB_RAW="$TMPDIR_WORK/symtab_raw.o"
objcopy -I binary -O elf32-i386 -B i386 \
    --rename-section .data=.symtab_blob \
    "$SYMTAB_BIN" "$SYMTAB_RAW"

# Determine the auto-generated symbol name prefix (depends on file path)
SYMTAB_PREFIX="$(readelf -s "$SYMTAB_RAW" | awk '/GLOBAL/ && /start/ {print $8; exit}')"
SYMTAB_BASE="${SYMTAB_PREFIX%_start}"

objcopy \
    --redefine-sym "${SYMTAB_BASE}_start=__symtab_start" \
    --redefine-sym "${SYMTAB_BASE}_end=__symtab_end" \
    "$SYMTAB_RAW" "$SYMTAB_O"

# ── .strtab blob ──────────────────────────────────────────────────────────────
STRTAB_RAW="$TMPDIR_WORK/strtab_raw.o"
objcopy -I binary -O elf32-i386 -B i386 \
    --rename-section .data=.strtab_blob \
    "$STRTAB_BIN" "$STRTAB_RAW"

STRTAB_PREFIX="$(readelf -s "$STRTAB_RAW" | awk '/GLOBAL/ && /start/ {print $8; exit}')"
STRTAB_BASE="${STRTAB_PREFIX%_start}"

objcopy \
    --redefine-sym "${STRTAB_BASE}_start=__strtab_start" \
    --redefine-sym "${STRTAB_BASE}_end=__strtab_end" \
    "$STRTAB_RAW" "$STRTAB_O"

echo "[embed_symtab] Generated: $SYMTAB_O  $STRTAB_O"
